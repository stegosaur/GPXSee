#ifndef TRACKPOINTITEM_H
#define TRACKPOINTITEM_H

#include <cmath>
#include <QTimeZone>
#include "data/path.h"
#include "map/map.h"
#include "format.h"
#include "graphicsscene.h"

/*
   One marker per recorded track point (used by the "Show waypoints" /
   "Audit waypoints" features). Unlike PathItem's generic click handling
   (which has to guess which point on the line was clicked), each
   TrackPointItem is built directly from a single known PathPoint, so the
   distance/time/moving-time it reports when clicked is always exactly
   correct for that point -- no nearest-point search involved.
*/
class TrackPointItem : public GraphicsItem
{
public:
	TrackPointItem(const PathPoint &point, const QDateTime &trackDate,
	  const QString &trackName, const QString &file, Map *map,
	  QGraphicsItem *parent = 0);

	const Coordinates &coordinates() const {return _point.coordinates();}
	qreal distance() const {return _point.distance();}

	void setMap(Map *map);
	void setSize(int size);
	void setColor(const QColor &color);
	void setDigitalZoom(int zoom) {setScale(pow(2, -zoom));}

	QRectF boundingRect() const;
	void paint(QPainter *painter, const QStyleOptionGraphicsItem *option,
	  QWidget *widget);

	ToolTip info(bool extended) const;

	static void setUnits(Units units) {_units = units;}
	static void setTimeZone(const QTimeZone &zone) {_timeZone = zone;}

protected:
	void hoverEnterEvent(QGraphicsSceneHoverEvent *event);
	void hoverLeaveEvent(QGraphicsSceneHoverEvent *event);
	void mousePressEvent(QGraphicsSceneMouseEvent *event);

private:
	PathPoint _point;
	QDateTime _trackDate;
	QString _trackName;
	QString _file;

	Map *_map;
	int _size;
	QColor _color;
	bool _hover;

	static Units _units;
	static QTimeZone _timeZone;
};

#endif // TRACKPOINTITEM_H
