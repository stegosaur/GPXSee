#include <QPainter>
#include <QGraphicsSceneMouseEvent>
#include <QCoreApplication>
#include <QCursor>
#include <QFileInfo>
#include <QLocale>
#include "popup.h"
#include "trackpointitem.h"

Units TrackPointItem::_units = Metric;
QTimeZone TrackPointItem::_timeZone = QTimeZone::utc();

TrackPointItem::TrackPointItem(const PathPoint &point,
  const QDateTime &trackDate, const QString &trackName, const QString &file,
  Map *map, QGraphicsItem *parent) : GraphicsItem(parent), _point(point),
  _trackDate(trackDate), _trackName(trackName), _file(file), _map(map)
{
	_size = 4;
	_color = Qt::black;
	_hover = false;

	setPos(map->ll2xy(point.coordinates()));
	setCursor(Qt::ArrowCursor);
	setAcceptHoverEvents(true);
	setZValue(1);
}

void TrackPointItem::setMap(Map *map)
{
	_map = map;
	setPos(map->ll2xy(_point.coordinates()));
}

void TrackPointItem::setSize(int size)
{
	prepareGeometryChange();
	_size = size;
}

void TrackPointItem::setColor(const QColor &color)
{
	_color = color;
	update();
}

QRectF TrackPointItem::boundingRect() const
{
	qreal s = (_hover ? _size * 1.5 : _size) + 1;
	return QRectF(-s/2, -s/2, s, s);
}

void TrackPointItem::paint(QPainter *painter,
  const QStyleOptionGraphicsItem *option, QWidget *widget)
{
	Q_UNUSED(option);
	Q_UNUSED(widget);

	qreal s = _hover ? _size * 1.5 : _size;

	painter->setPen(Qt::NoPen);
	painter->setBrush(QBrush(_color, Qt::SolidPattern));
	painter->drawEllipse(QRectF(-s/2, -s/2, s, s));
}

void TrackPointItem::hoverEnterEvent(QGraphicsSceneHoverEvent *event)
{
	Q_UNUSED(event);

	prepareGeometryChange();
	_hover = true;
	setZValue(zValue() + 1.0);
}

void TrackPointItem::hoverLeaveEvent(QGraphicsSceneHoverEvent *event)
{
	Q_UNUSED(event);

	prepareGeometryChange();
	_hover = false;
	setZValue(zValue() - 1.0);
}

void TrackPointItem::mousePressEvent(QGraphicsSceneMouseEvent *event)
{
	GraphicsScene *gs = dynamic_cast<GraphicsScene *>(scene());
	if (gs)
		Popup::show(event->screenPos(), info(gs->showExtendedInfo()),
		  event->widget());

	event->accept();
}

ToolTip TrackPointItem::info(bool extended) const
{
	ToolTip tt;
	QLocale l;

	if (!_trackName.isEmpty())
		tt.insert(QCoreApplication::translate("TrackPointItem", "Track"),
		  _trackName);
	tt.insert(QCoreApplication::translate("TrackPointItem", "Distance"),
	  Format::distance(_point.distance(), _units));
	if (!std::isnan(_point.time()) && _point.time() > 0)
		tt.insert(QCoreApplication::translate("TrackPointItem", "Total time"),
		  Format::timeSpan(_point.time()));
	if (!std::isnan(_point.movingTime()) && _point.movingTime() > 0)
		tt.insert(QCoreApplication::translate("TrackPointItem", "Moving time"),
		  Format::timeSpan(_point.movingTime()));

	QDateTime pointDate((!std::isnan(_point.time()) && !_trackDate.isNull())
	  ? _trackDate.addMSecs(qRound64(_point.time() * 1000.0)) : QDateTime());
	if (!pointDate.isNull()) {
		QDateTime date(pointDate.toTimeZone(_timeZone));
		tt.insert(QCoreApplication::translate("TrackPointItem", "Date"),
		  l.toString(date.date(), QLocale::ShortFormat) + " "
		  + date.time().toString("h:mm:ss"));
	}

#ifndef Q_OS_ANDROID
	if (extended && !_file.isEmpty())
		tt.insert(QCoreApplication::translate("TrackPointItem", "File"),
		  QString("<a href=\"file:%1\">%2</a>").arg(_file,
		  QFileInfo(_file).fileName()));
#else
	Q_UNUSED(extended);
#endif // Q_OS_ANDROID

	return tt;
}
