#ifndef WAYPOINTAUDIT_H
#define WAYPOINTAUDIT_H

#include <QDockWidget>
#include <QTimeZone>
#include <QDateTime>
#include <QSet>
#include "common/coordinates.h"
#include "data/path.h"

class QTreeWidget;
class QTreeWidgetItem;
class QComboBox;
class Track;

/*
   Left-hand sidebar ("Data > Audit waypoints") listing every recorded
   track point across all open files, grouped by file (and by track, if a
   file has more than one), then by hour, in collapsible sections, with
   each point labeled by time (HH:mm:ss). Selecting an entry -- by click
   or by keyboard up/down -- re-centers the map on that point and moves
   the map/graph markers to it. A combo box lets the displayed times (and
   hour grouping) be viewed in any timezone, independent of the app's
   global timezone setting.
*/
class WaypointAudit : public QDockWidget
{
	Q_OBJECT

public:
	WaypointAudit(QWidget *parent = 0);

	void addFile(const QString &fileName, const QList<Track> &tracks);
	void removeFile(const QString &fileName);
	void clear();

signals:
	void pointActivated(const Coordinates &coordinates, qreal distance);
	void fileCloseRequested(const QString &fileName);
	/* The set of files checked via the "Highlight" context-menu entry.
	   Empty set == no highlight active (all files shown). */
	void highlightChanged(const QSet<QString> &files);

private slots:
	void handleCurrentItemChanged(QTreeWidgetItem *current,
	  QTreeWidgetItem *previous);
	void timeZoneSelected(int index);
	void useSystemTimeZone();
	void showContextMenu(const QPoint &pos);

private:
	struct TrackEntry {
		QString file;
		QString name;
		QDateTime date;
		Path points;
	};

	void rebuild();
	void activateItem(QTreeWidgetItem *item);

	QTreeWidget *_tree;
	QComboBox *_timeZoneCombo;
	QList<TrackEntry> _entries;
	QTimeZone _timeZone;
	QSet<QString> _highlighted;
};

#endif // WAYPOINTAUDIT_H
