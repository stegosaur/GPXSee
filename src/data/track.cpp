#include "map/map.h"
#include "track.h"


int Track::_elevationWindow = 3;
int Track::_speedWindow = 5;
int Track::_heartRateWindow = 3;
int Track::_cadenceWindow = 3;
int Track::_powerWindow = 3;

bool Track::_detectPauses = true;
bool Track::_automaticPause = true;
qreal Track::_pauseSpeed = 0.5;
int Track::_pauseInterval = 10;

bool Track::_outlierEliminate = true;
bool Track::_useReportedSpeed = false;
bool Track::_useDEM = false;
bool Track::_show2ndElevation = false;
bool Track::_show2ndSpeed = false;
bool Track::_useSegments = true;

static qreal avg(const QVector<qreal> &v)
{
	qreal sum = 0;

	for (int i = 0; i < v.size(); i++)
		sum += v.at(i);

	return sum/v.size();
}

static qreal median(QVector<qreal> &v)
{
	std::sort(v.begin(), v.end());
	return v.at(v.size() / 2);
}

/*
   Median point-to-point time delta (seconds) in a segment. Used to scale
   the pause-detection interval to the segment's actual sampling rate: a
   fixed short pauseInterval (e.g. the 10s automatic default) implicitly
   assumes logging at roughly 1Hz. For coarser-interval loggers (e.g. a
   phone logging every ~2 minutes to save battery), a single low-speed
   reading between two consecutive points would already exceed a 10s
   threshold and get misclassified as a stop, collapsing most of the track
   into "stopped" runs that get stripped from the rendered path/distance.
*/
static qreal medianInterval(const QVector<qreal> &time)
{
	if (time.size() < 2)
		return NAN;

	QVector<qreal> dt(time.size() - 1);
	for (int i = 1; i < time.size(); i++)
		dt[i - 1] = time.at(i) - time.at(i - 1);

	return median(dt);
}

static qreal MAD(QVector<qreal> &v, qreal m)
{
	for (int i = 0; i < v.size(); i++)
		v[i] = qAbs(v.at(i) - m);
	return median(v);
}

/*
   Modified Z-score (Iglewicz and Hoaglin)
   The acceleration data distribution has usualy a (much) higher kurtosis than
   the normal distribution thus a higher comparsion value than the usual 3.5 is
   required.

   On a track that is mostly stationary with only brief bursts of real
   movement (e.g. a coarse-interval logger that runs for many hours), the
   acceleration distribution is dominated by the near-zero stationary
   baseline, so a real movement burst -- even a completely genuine one --
   looks like a statistical outlier by comparison. A pure global Z-score
   test would then strip the entire real trip out of both the path and the
   distance.

   Confirm each Z-score-flagged candidate against physical plausibility
   instead of pure statistics: only actually eliminate it if reaching it
   or leaving it requires a speed no real GPS track should show (a
   genuine glitch/teleport). A geometric "does skipping it shortcut the
   path" test was tried first but wrongly caught real out-and-back/turn
   points (e.g. a dead-end or cul-de-sac) that are statistically rare but
   physically completely ordinary -- speed plausibility doesn't have that
   blind spot, since real travel, even a sharp turnaround, still moves at
   an ordinary speed on both legs.
*/
static QSet<int> eliminate(const SegmentData &sd, const QVector<qreal> &v)
{
	static const qreal MAX_PLAUSIBLE_SPEED = 60.0; // m/s (~216 km/h)

	QSet<int> rm;

	QVector<qreal> w(v);
	qreal m = median(w);
	qreal M = MAD(w, m);

	for (int i = 0; i < v.size(); i++) {
		if (M <= 0 || qAbs((0.6745 * (v.at(i) - m)) / M) <= 5.0)
			continue;

		if (i > 0 && i < sd.size() - 1) {
			qreal dIn = sd.at(i-1).coordinates().distanceTo(
			  sd.at(i).coordinates());
			qreal dOut = sd.at(i).coordinates().distanceTo(
			  sd.at(i+1).coordinates());
			qreal dtIn = sd.at(i-1).timestamp().msecsTo(
			  sd.at(i).timestamp()) / 1000.0;
			qreal dtOut = sd.at(i).timestamp().msecsTo(
			  sd.at(i+1).timestamp()) / 1000.0;
			qreal vIn = (dtIn > 1e-3) ? dIn / dtIn : 0.0;
			qreal vOut = (dtOut > 1e-3) ? dOut / dtOut : 0.0;

			if (vIn <= MAX_PLAUSIBLE_SPEED && vOut <= MAX_PLAUSIBLE_SPEED)
				continue;
		}

		rm.insert(i);
	}

	return rm;
}

static GraphSegment filter(const GraphSegment &g, int window)
{
	if (g.size() < window || window < 2)
		return g;

	qreal acc = 0;
	GraphSegment ret(g.size(), g.start());

	for (int i = 0; i < window; i++)
		acc += g.at(i).y();
	for (int i = 0; i <= window/2; i++)
		ret[i] = GraphPoint(g.at(i).s(), g.at(i).t(), acc/window);

	for (int i = window/2 + 1; i < g.size() - window/2; i++) {
		acc += g.at(i + window/2).y() - g.at(i - (window/2 + 1)).y();
		ret[i] = GraphPoint(g.at(i).s(), g.at(i).t(), acc/window);
	}

	for (int i = g.size() - window/2; i < g.size(); i++)
		ret[i] = GraphPoint(g.at(i).s(), g.at(i).t(), acc/window);

	return ret;
}


qreal Track::lastDistance(int seg)
{
	for (int i = seg - 1; i >= 0; i--)
		if (!_segments.at(i).distance.isEmpty())
			return _segments.at(i).distance.last();

	return 0;
}

qreal Track::lastTime(int seg)
{
	for (int i = seg - 1; i >= 0; i--)
		if (!_segments.at(i).time.isEmpty())
			return _segments.at(i).time.last();

	return 0;
}

Track::Track(const TrackData &data) : _pause(0)
{
	qreal ds, dt;

	if (_useSegments)
		_data = data;
	else {
		if (!data.isEmpty()) {
			_data.append(data.first());
			for (int i = 1; i < data.size(); i++)
				_data.first() << data.at(i);
		}
	}

	for (int i = 0; i < _data.size(); i++) {
		const SegmentData &sd = _data.at(i);
		_segments.append(Segment());
		if (sd.isEmpty())
			continue;

		// precompute distances, times, speeds and acceleration
		QVector<qreal> acceleration;

		Segment &seg = _segments.last();

		seg.start = sd.first().timestamp();
		seg.distance.append(lastDistance(i));
		seg.time.append(sd.first().hasTimestamp() ? lastTime(i) : NAN);
		seg.speed.append(sd.first().hasTimestamp() ? 0 : NAN);
		acceleration.append(sd.first().hasTimestamp() ? 0 : NAN);
		bool hasTime = !std::isnan(seg.time.first());

		for (int j = 1; j < sd.size(); j++) {
			ds = sd.at(j).coordinates().distanceTo(sd.at(j-1).coordinates());
			seg.distance.append(seg.distance.last() + ds);

			if (hasTime && sd.at(j).timestamp().isValid()) {
				if (sd.at(j).timestamp() > sd.at(j-1).timestamp())
					dt = sd.at(j-1).timestamp().msecsTo(
					  sd.at(j).timestamp()) / 1000.0;
				else {
					qWarning("%s: %s: time skew detected",
					  qUtf8Printable(_data.file()),
					  qUtf8Printable(sd.at(j).timestamp().toString(Qt::ISODate)));
					dt = 0;
				}
			} else {
				dt = NAN;
				if (hasTime) {
					qWarning("%s: missing timestamp(s), time graphs disabled",
					  qUtf8Printable(_data.file()));
					hasTime = false;
					for (int i = 0; i < seg.time.size(); i++)
						seg.time[i] = NAN;
					for (int i = 0; i < seg.speed.size(); i++)
						seg.speed[i] = NAN;
				}
			}
			seg.time.append(seg.time.last() + dt);

			if (dt < 1e-3) {
				seg.speed.append(seg.speed.last());
				acceleration.append(acceleration.last());
			} else {
				qreal v = ds / dt;
				qreal dv = v - seg.speed.last();
				seg.speed.append(v);
				acceleration.append(dv / dt);
			}
		}

		if (!hasTime)
			continue;

		// Default: no pause info available yet, moving time == elapsed
		// time. Overwritten below once pause detection has run.
		seg.movingTime = seg.time;

		if (_detectPauses) {
			// get stop-points + pause duration
			int pauseInterval;
			qreal pauseSpeed;

			if (_automaticPause) {
				pauseSpeed = (avg(seg.speed) > 2.8) ? 0.40 : 0.15;
				pauseInterval = 10;
			} else {
				pauseSpeed = _pauseSpeed;
				pauseInterval = _pauseInterval;
			}

			qreal medianDt = medianInterval(seg.time);
			if (!std::isnan(medianDt))
				pauseInterval = qMax(pauseInterval,
				  (int)std::ceil(medianDt * 1.5));

			int ss = 0, la = 0;
			for (int j = 1; j < seg.time.size(); j++) {
				if (seg.speed.at(j) > pauseSpeed)
					ss = -1;
				else if (ss < 0)
					ss = j-1;

				if (ss >= 0 && seg.time.at(j) > seg.time.at(ss) + pauseInterval) {
					int l = qMax(ss, la);
					_pause += seg.time.at(j) - seg.time.at(l);
					for (int k = l; k <= j; k++)
						seg.stop.insert(k);
					la = j;
				}
			}

			// Per-point cumulative moving time: elapsed time minus every
			// point-to-point interval that falls entirely within a
			// confirmed stop run. Summed over the whole segment this
			// reconstructs the same total as _pause above.
			qreal cumPause = 0;
			for (int j = 1; j < seg.time.size(); j++) {
				if (seg.stop.contains(j) && seg.stop.contains(j-1))
					cumPause += seg.time.at(j) - seg.time.at(j-1);
				seg.movingTime[j] = seg.time.at(j) - cumPause;
			}
		}

		if (!_outlierEliminate)
			continue;


		// eliminate outliers
		seg.outliers = eliminate(sd, acceleration);

		// stop-points can not be outliers
		QSet<int>::const_iterator it;
		for (it = seg.stop.constBegin(); it != seg.stop.constEnd(); ++it)
			seg.outliers.remove(*it);

		// recompute distances (and dependand data) without outliers
		int last = 0;
		for (int j = 0; j < sd.size(); j++) {
			if (seg.outliers.contains(j))
				last++;
			else
				break;
		}
		for (int j = last + 1; j < sd.size(); j++) {
			if (seg.outliers.contains(j))
				continue;
			if (discardStopPoint(seg, j)) {
				seg.distance[j] = seg.distance.at(last);
				seg.speed[j] = 0;
			} else {
				ds = sd.at(j).coordinates().distanceTo(
				  sd.at(last).coordinates());
				seg.distance[j] = seg.distance.at(last) + ds;

				dt = seg.time.at(j) - seg.time.at(last);
				seg.speed[j] = (dt < 1e-3) ? seg.speed.at(last) : ds / dt;
			}
			last = j;
		}
	}
}

Graph Track::gpsElevation() const
{
	Graph ret;

	for (int i = 0; i < _data.size(); i++) {
		const SegmentData &sd = _data.at(i);
		if (sd.size() < 2)
			continue;
		const Segment &seg = _segments.at(i);
		GraphSegment gs(seg.start);

		for (int j = 0; j < sd.size(); j++) {
			if (!sd.at(j).hasElevation() || seg.outliers.contains(j))
				continue;
			gs.append(GraphPoint(seg.distance.at(j), seg.time.at(j),
			  sd.at(j).elevation()));
		}

		if (gs.size() >= 2)
			ret.append(filter(gs, _elevationWindow));
	}

	if (_data.style().color().isValid())
		ret.setColor(_data.style().color());

	return ret;
}

Graph Track::demElevation(Map *map) const
{
	Graph ret;

	for (int i = 0; i < _data.size(); i++) {
		const SegmentData &sd = _data.at(i);
		if (sd.size() < 2)
			continue;
		const Segment &seg = _segments.at(i);
		GraphSegment gs(seg.start);

		for (int j = 0; j < sd.size(); j++) {
			qreal dem = map->elevation(sd.at(j).coordinates());
			if (std::isnan(dem) || seg.outliers.contains(j))
				continue;
			gs.append(GraphPoint(seg.distance.at(j), seg.time.at(j), dem));
		}

		if (gs.size() >= 2)
			ret.append(filter(gs, _elevationWindow));
	}

	if (_data.style().color().isValid())
		ret.setColor(_data.style().color());

	return ret;
}

GraphPair Track::elevation(Map *map) const
{
	if (_useDEM) {
		Graph dem(demElevation(map));
		return (dem.isEmpty())
		  ? GraphPair(gpsElevation(), Graph())
		  : GraphPair(dem, _show2ndElevation ? gpsElevation() : Graph());
	} else {
		Graph gps(gpsElevation());
		return (gps.isEmpty())
		  ? GraphPair(demElevation(map), Graph())
		  : GraphPair(gps, _show2ndElevation ? demElevation(map) : Graph());
	}
}

Graph Track::computedSpeed() const
{
	Graph ret;

	for (int i = 0; i < _data.size(); i++) {
		const SegmentData &sd = _data.at(i);
		if (sd.size() < 2)
			continue;
		const Segment &seg = _segments.at(i);
		GraphSegment gs(seg.start);
		QList<int> stop;
		qreal v;

		for (int j = 0; j < sd.size(); j++) {
			if (seg.stop.contains(j) && !std::isnan(seg.speed.at(j))) {
				v = 0;
				stop.append(gs.size());
			} else if (!std::isnan(seg.speed.at(j)) && !seg.outliers.contains(j))
				v = seg.speed.at(j);
			else
				continue;

			gs.append(GraphPoint(seg.distance.at(j), seg.time.at(j), v));
		}

		if (gs.size() >= 2) {
			ret.append(filter(gs, _speedWindow));
			GraphSegment &filtered = ret.last();

			for (int j = 0; j < stop.size(); j++)
				filtered[stop.at(j)].setY(0);
		}
	}

	if (_data.style().color().isValid())
		ret.setColor(_data.style().color());

	return ret;
}

Graph Track::reportedSpeed() const
{
	Graph ret;

	for (int i = 0; i < _data.size(); i++) {
		const SegmentData &sd = _data.at(i);
		if (sd.size() < 2)
			continue;
		const Segment &seg = _segments.at(i);
		GraphSegment gs(seg.start);
		QList<int> stop;
		qreal v;

		for (int j = 0; j < sd.size(); j++) {
			if (seg.stop.contains(j) && sd.at(j).hasSpeed()) {
				v = 0;
				stop.append(gs.size());
			} else if (sd.at(j).hasSpeed() && !seg.outliers.contains(j))
				v = sd.at(j).speed();
			else
				continue;

			gs.append(GraphPoint(seg.distance.at(j), seg.time.at(j), v));
		}

		if (gs.size() >= 2) {
			ret.append(filter(gs, _speedWindow));
			GraphSegment &filtered = ret.last();

			for (int j = 0; j < stop.size(); j++)
				filtered[stop.at(j)].setY(0);
		}
	}

	if (_data.style().color().isValid())
		ret.setColor(_data.style().color());

	return ret;
}

GraphPair Track::speed() const
{
	if (_useReportedSpeed) {
		Graph reported(reportedSpeed());
		return (reported.isEmpty())
		  ? GraphPair(computedSpeed(), Graph())
		  : GraphPair(reported, _show2ndSpeed ? computedSpeed() : Graph());
	} else {
		Graph computed(computedSpeed());
		return (computed.isEmpty())
		  ? GraphPair(reportedSpeed(), Graph())
		  : GraphPair(computed, _show2ndSpeed ? reportedSpeed() : Graph());
	}
}

Graph Track::heartRate() const
{
	Graph ret;

	for (int i = 0; i < _data.size(); i++) {
		const SegmentData &sd = _data.at(i);
		if (sd.size() < 2)
			continue;
		const Segment &seg = _segments.at(i);
		GraphSegment gs(seg.start);

		for (int j = 0; j < sd.size(); j++)
			if (sd.at(j).hasHeartRate() && !seg.outliers.contains(j))
				gs.append(GraphPoint(seg.distance.at(j), seg.time.at(j),
				  sd.at(j).heartRate()));

		if (gs.size() >= 2)
			ret.append(filter(gs, _heartRateWindow));
	}

	if (_data.style().color().isValid())
		ret.setColor(_data.style().color());

	return ret;
}

Graph Track::temperature() const
{
	Graph ret;

	for (int i = 0; i < _data.size(); i++) {
		const SegmentData &sd = _data.at(i);
		if (sd.size() < 2)
			continue;
		const Segment &seg = _segments.at(i);
		GraphSegment gs(seg.start);

		for (int j = 0; j < sd.count(); j++) {
			if (sd.at(j).hasTemperature() && !seg.outliers.contains(j))
				gs.append(GraphPoint(seg.distance.at(j), seg.time.at(j),
				  sd.at(j).temperature()));
		}

		if (gs.size() >= 2)
			ret.append(gs);
	}

	if (_data.style().color().isValid())
		ret.setColor(_data.style().color());

	return ret;
}

Graph Track::ratio() const
{
	Graph ret;

	for (int i = 0; i < _data.size(); i++) {
		const SegmentData &sd = _data.at(i);
		if (sd.size() < 2)
			continue;
		const Segment &seg = _segments.at(i);
		GraphSegment gs(seg.start);

		for (int j = 0; j < sd.size(); j++)
			if (sd.at(j).hasRatio() && !seg.outliers.contains(j))
				gs.append(GraphPoint(seg.distance.at(j), seg.time.at(j),
				  sd.at(j).ratio()));

		if (gs.size() >= 2)
			ret.append(gs);
	}

	if (_data.style().color().isValid())
		ret.setColor(_data.style().color());

	return ret;
}

Graph Track::cadence() const
{
	Graph ret;

	for (int i = 0; i < _data.size(); i++) {
		const SegmentData &sd = _data.at(i);
		if (sd.size() < 2)
			continue;
		const Segment &seg = _segments.at(i);
		GraphSegment gs(seg.start);
		QList<int> stop;
		qreal c;

		for (int j = 0; j < sd.size(); j++) {
			if (sd.at(j).hasCadence() && seg.stop.contains(j)) {
				c = 0;
				stop.append(gs.size());
			} else if (sd.at(j).hasCadence() && !seg.outliers.contains(j))
				c = sd.at(j).cadence();
			else
				continue;

			gs.append(GraphPoint(seg.distance.at(j), seg.time.at(j), c));
		}

		if (gs.size() >= 2) {
			ret.append(filter(gs, _cadenceWindow));
			GraphSegment &filtered = ret.last();

			for (int j = 0; j < stop.size(); j++)
				filtered[stop.at(j)].setY(0);
		}
	}

	if (_data.style().color().isValid())
		ret.setColor(_data.style().color());

	return ret;
}

Graph Track::power() const
{
	Graph ret;
	QList<int> stop;
	qreal p;


	for (int i = 0; i < _data.size(); i++) {
		const SegmentData &sd = _data.at(i);
		if (sd.size() < 2)
			continue;
		const Segment &seg = _segments.at(i);
		GraphSegment gs(seg.start);

		for (int j = 0; j < sd.size(); j++) {
			if (sd.at(j).hasPower() && seg.stop.contains(j)) {
				p = 0;
				stop.append(gs.size());
			} else if (sd.at(j).hasPower() && !seg.outliers.contains(j))
				p = sd.at(j).power();
			else
				continue;

			gs.append(GraphPoint(seg.distance.at(j), seg.time.at(j), p));
		}

		if (gs.size() >= 2) {
			ret.append(filter(gs, _powerWindow));
			GraphSegment &filtered = ret.last();

			for (int j = 0; j < stop.size(); j++)
				filtered[stop.at(j)].setY(0);
		}
	}

	if (_data.style().color().isValid())
		ret.setColor(_data.style().color());

	return ret;
}

qreal Track::distance() const
{
	for (int i = _segments.size() - 1; i >= 0; i--) {
		const Segment &seg = _segments.at(i);

		for (int j = seg.distance.size() - 1; j >= 0; j--)
			if (!seg.outliers.contains(j))
				return seg.distance.at(j);
	}

	return 0;
}

qreal Track::time() const
{
	for (int i = _segments.size() - 1; i >= 0; i--) {
		const Segment &seg = _segments.at(i);

		for (int j = seg.time.size() - 1; j >= 0; j--)
			if (!seg.outliers.contains(j))
				return seg.time.at(j);
	}

	return 0;
}

qreal Track::movingTime() const
{
	return (time() - _pause);
}

QDateTime Track::date() const
{
	return (_data.size() && _data.first().size())
	  ? _data.first().first().timestamp() : QDateTime();
}

Path Track::path() const
{
	Path ret;

	for (int i = 0; i < _data.size(); i++) {
		const SegmentData &sd = _data.at(i);
		if (sd.size() < 2)
			continue;
		const Segment &seg = _segments.at(i);
		ret.append(PathSegment());
		PathSegment &ps = ret.last();

		for (int j = 0; j < sd.size(); j++)
			if (!seg.outliers.contains(j) && !discardStopPoint(seg, j))
				ps.append(PathPoint(sd.at(j).coordinates(),
				  seg.distance.at(j), seg.time.at(j),
				  seg.movingTime.isEmpty() ? NAN : seg.movingTime.at(j)));
	}

	ret.setStyle(_data.style());

	return ret;
}

Path Track::allPoints() const
{
	Path ret;

	for (int i = 0; i < _data.size(); i++) {
		const SegmentData &sd = _data.at(i);
		if (sd.isEmpty())
			continue;
		const Segment &seg = _segments.at(i);
		ret.append(PathSegment());
		PathSegment &ps = ret.last();

		for (int j = 0; j < sd.size(); j++)
			ps.append(PathPoint(sd.at(j).coordinates(), seg.distance.at(j),
			  seg.time.at(j), seg.movingTime.isEmpty() ? NAN
			  : seg.movingTime.at(j)));
	}

	ret.setStyle(_data.style());

	return ret;
}

bool Track::discardStopPoint(const Segment &seg, int i) const
{
	return (seg.stop.contains(i) && seg.stop.contains(i-1)
	  && seg.stop.contains(i+1) && i > 0 && i < seg.distance.size() - 1);
}

bool Track::isValid() const
{
	for (int i = 0; i < _data.size(); i++)
		if (_data.at(i).size() >= 2)
			return true;
	return false;
}
