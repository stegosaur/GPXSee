#include <cmath>
#include <algorithm>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTreeWidget>
#include <QComboBox>
#include <QPushButton>
#include <QWidget>
#include <QFileInfo>
#include <QHash>
#include <QMenu>
#include "data/track.h"
#include "waypointaudit.h"

#define DISTANCE_ROLE (Qt::UserRole)
#define COORDINATES_ROLE (Qt::UserRole + 1)
#define FILE_ROLE (Qt::UserRole + 2)

WaypointAudit::WaypointAudit(QWidget *parent)
  : QDockWidget(tr("Audit waypoints"), parent)
{
	_timeZone = QTimeZone::systemTimeZone();

	QWidget *container = new QWidget(this);
	QVBoxLayout *layout = new QVBoxLayout(container);
	layout->setContentsMargins(4, 4, 4, 4);

	QHBoxLayout *tzLayout = new QHBoxLayout();
	_timeZoneCombo = new QComboBox(container);
	/* Plain UTC offsets instead of the (huge) IANA zone list, each with
	   a representative major city. Fixed offsets -- no DST. The item's
	   user data is the offset from UTC in seconds. */
	static const struct {
		int hours;
		int minutes;
		const char *city;
	} zones[] = {
		{-12, 0, 0}, {-11, 0, "Pago Pago"}, {-10, 0, "Honolulu"},
		{-9, 0, "Anchorage"}, {-8, 0, "Los Angeles"}, {-7, 0, "Denver"},
		{-6, 0, "Chicago"}, {-5, 0, "New York"}, {-4, 0, "Caracas"},
		{-3, 0, "Buenos Aires"}, {-2, 0, 0}, {-1, 0, "Azores"},
		{0, 0, "London"}, {1, 0, "Paris"}, {2, 0, "Cairo"},
		{3, 0, "Moscow"}, {4, 0, "Dubai"}, {5, 0, "Karachi"},
		{5, 30, "Mumbai"}, {6, 0, "Dhaka"}, {7, 0, "Bangkok"},
		{8, 0, "Beijing"}, {9, 0, "Tokyo"}, {9, 30, "Adelaide"},
		{10, 0, "Sydney"}, {11, 0, "Noumea"}, {12, 0, "Auckland"},
		{13, 0, "Apia"}, {14, 0, "Kiritimati"}
	};
	for (size_t i = 0; i < sizeof(zones) / sizeof(zones[0]); i++) {
		int offset = zones[i].hours * 3600
		  + ((zones[i].hours < 0) ? -1 : 1) * zones[i].minutes * 60;

		QString label(QLatin1String("UTC"));
		if (offset) {
			label += (offset > 0) ? "+" : "-";
			label += QString::number(qAbs(zones[i].hours));
			if (zones[i].minutes)
				label += QString(":%1").arg(zones[i].minutes, 2, 10,
				  QChar('0'));
		}
		if (zones[i].city)
			label += QString(" (%1)").arg(zones[i].city);

		_timeZoneCombo->addItem(label, offset);
	}
	int current = _timeZoneCombo->findData(
	  _timeZone.offsetFromUtc(QDateTime::currentDateTimeUtc()));
	if (current >= 0) {
		_timeZoneCombo->setCurrentIndex(current);
		_timeZone = QTimeZone(_timeZoneCombo->itemData(current).toInt());
	}
	QPushButton *systemTzButton = new QPushButton(tr("Use system timezone"),
	  container);
	tzLayout->addWidget(_timeZoneCombo, 1);
	tzLayout->addWidget(systemTzButton);
	layout->addLayout(tzLayout);

	_tree = new QTreeWidget(container);
	_tree->setHeaderHidden(true);
	_tree->setColumnCount(1);
	_tree->setUniformRowHeights(true);
	_tree->setContextMenuPolicy(Qt::CustomContextMenu);
	layout->addWidget(_tree);

	container->setLayout(layout);
	setWidget(container);
	setObjectName("waypointAudit");

	connect(_tree, &QTreeWidget::currentItemChanged, this,
	  &WaypointAudit::handleCurrentItemChanged);
	connect(_tree, &QTreeWidget::customContextMenuRequested, this,
	  &WaypointAudit::showContextMenu);
	connect(_timeZoneCombo,
	  QOverload<int>::of(&QComboBox::currentIndexChanged), this,
	  &WaypointAudit::timeZoneSelected);
	connect(systemTzButton, &QPushButton::clicked, this,
	  &WaypointAudit::useSystemTimeZone);
}

void WaypointAudit::addFile(const QString &fileName, const QList<Track> &tracks)
{
	/* Highlight toggling reloads the open files through the normal load
	   path without clearing the sidebar, so re-adding an already listed
	   file must be a no-op. */
	for (int i = 0; i < _entries.size(); i++)
		if (_entries.at(i).file == fileName)
			return;

	for (int i = 0; i < tracks.count(); i++) {
		const Track &track = tracks.at(i);
		Path points(track.allPoints());
		if (points.isEmpty())
			continue;

		TrackEntry entry;
		entry.file = fileName;
		entry.name = track.name();
		entry.date = track.date();
		entry.points = points;
		_entries.append(entry);
	}

	rebuild();
}

void WaypointAudit::removeFile(const QString &fileName)
{
	for (int i = _entries.size() - 1; i >= 0; i--)
		if (_entries.at(i).file == fileName)
			_entries.removeAt(i);
	_highlighted.remove(fileName);

	rebuild();
}

void WaypointAudit::clear()
{
	_entries.clear();
	_highlighted.clear();
	_tree->clear();
}

void WaypointAudit::rebuild()
{
	_tree->clear();

	QHash<QString, int> trackCountPerFile;
	for (int e = 0; e < _entries.size(); e++)
		trackCountPerFile[_entries.at(e).file]++;

	QHash<QString, QTreeWidgetItem*> sectionItems;
	QHash<QString, QTreeWidgetItem*> hourItems;
	QHash<QString, int> sectionCounts;
	QStringList sectionOrder;

	for (int e = 0; e < _entries.size(); e++) {
		const TrackEntry &entry = _entries.at(e);
		QString fileLabel(QFileInfo(entry.file).fileName());
		bool multi = trackCountPerFile.value(entry.file) > 1;
		QString sectionKey = entry.file + (multi ? "|" + entry.name : QString());
		QString sectionLabel = multi
		  ? fileLabel + (entry.name.isEmpty() ? QString() : " - " + entry.name)
		  : fileLabel;

		QTreeWidgetItem *sectionItem = sectionItems.value(sectionKey);
		if (!sectionItem) {
			sectionItem = new QTreeWidgetItem(_tree, QStringList(sectionLabel));
			sectionItem->setFlags(sectionItem->flags() & ~Qt::ItemIsSelectable);
			sectionItem->setData(0, FILE_ROLE, entry.file);
			if (_highlighted.contains(entry.file)) {
				QFont font(sectionItem->font(0));
				font.setBold(true);
				sectionItem->setFont(0, font);
			}
			sectionItems.insert(sectionKey, sectionItem);
			sectionCounts.insert(sectionKey, 0);
			sectionOrder.append(sectionKey);
		}

		for (int s = 0; s < entry.points.size(); s++) {
			const PathSegment &seg = entry.points.at(s);
			for (int j = 0; j < seg.size(); j++) {
				const PathPoint &pt = seg.at(j);

				QDateTime abs;
				if (!std::isnan(pt.time()) && !entry.date.isNull())
					abs = entry.date.addMSecs(qRound64(pt.time() * 1000.0));

				QDateTime local(abs.isValid() ? abs.toTimeZone(_timeZone)
				  : QDateTime());

				QString hourLabel = local.isValid()
				  ? local.toString("MMM d, HH:00") : tr("Unknown time");
				QString hourKey = sectionKey + "|" + hourLabel;

				QTreeWidgetItem *hourItem = hourItems.value(hourKey);
				if (!hourItem) {
					hourItem = new QTreeWidgetItem(sectionItem,
					  QStringList(hourLabel));
					hourItem->setData(0, FILE_ROLE, entry.file);
					// Navigating to the hour (click or keyboard) jumps to
					// the first point recorded in it.
					hourItem->setData(0, DISTANCE_ROLE, pt.distance());
					hourItem->setData(0, COORDINATES_ROLE,
					  QPointF(pt.coordinates().lon(), pt.coordinates().lat()));
					hourItems.insert(hourKey, hourItem);
				}

				QString text = local.isValid()
				  ? local.time().toString("HH:mm:ss") : tr("Point %1").arg(j + 1);

				QTreeWidgetItem *item = new QTreeWidgetItem(hourItem,
				  QStringList(text));
				item->setData(0, FILE_ROLE, entry.file);
				item->setData(0, DISTANCE_ROLE, pt.distance());
				item->setData(0, COORDINATES_ROLE,
				  QPointF(pt.coordinates().lon(), pt.coordinates().lat()));

				sectionCounts[sectionKey]++;
			}
		}
	}

	for (int i = 0; i < sectionOrder.size(); i++) {
		const QString &key = sectionOrder.at(i);
		QTreeWidgetItem *item = sectionItems.value(key);
		item->setText(0, item->text(0) + QString(" (%1)")
		  .arg(sectionCounts.value(key)));
	}

	for (int i = 0; i < _tree->topLevelItemCount(); i++)
		_tree->topLevelItem(i)->setExpanded(true);
}

void WaypointAudit::activateItem(QTreeWidgetItem *item)
{
	if (!item)
		return;

	QVariant cv(item->data(0, COORDINATES_ROLE));
	if (!cv.isValid())
		return;

	QPointF ll(cv.toPointF());
	qreal distance = item->data(0, DISTANCE_ROLE).toDouble();

	emit pointActivated(Coordinates(ll.x(), ll.y()), distance);
}

void WaypointAudit::handleCurrentItemChanged(QTreeWidgetItem *current,
  QTreeWidgetItem *previous)
{
	Q_UNUSED(previous);
	activateItem(current);
}

void WaypointAudit::timeZoneSelected(int index)
{
	if (index < 0)
		return;

	_timeZone = QTimeZone(_timeZoneCombo->itemData(index).toInt());
	rebuild();
}

void WaypointAudit::useSystemTimeZone()
{
	/* Map the system zone to its current UTC offset (the offset list has
	   no DST, so this picks the offset valid right now). */
	int offset = QTimeZone::systemTimeZone().offsetFromUtc(
	  QDateTime::currentDateTimeUtc());
	int idx = _timeZoneCombo->findData(offset);
	if (idx >= 0)
		_timeZoneCombo->setCurrentIndex(idx);
}

void WaypointAudit::showContextMenu(const QPoint &pos)
{
	QTreeWidgetItem *item = _tree->itemAt(pos);
	QString file;
	if (item) {
		QVariant fv(item->data(0, FILE_ROLE));
		if (fv.isValid())
			file = fv.toString();
	}

	QMenu menu(_tree);
	QAction *highlightAction = 0, *closeAction = 0;
	if (!file.isEmpty()) {
		highlightAction = menu.addAction(
		  tr("Highlight %1").arg(QFileInfo(file).fileName()));
		highlightAction->setCheckable(true);
		highlightAction->setChecked(_highlighted.contains(file));
		closeAction = menu.addAction(
		  tr("Close %1").arg(QFileInfo(file).fileName()));
		menu.addSeparator();
	}
	QAction *clearHighlightsAction = menu.addAction(
	  tr("Remove all highlights"));
	clearHighlightsAction->setEnabled(!_highlighted.isEmpty());

	QAction *chosen = menu.exec(_tree->viewport()->mapToGlobal(pos));
	if (!chosen)
		return;

	if (chosen == closeAction)
		emit fileCloseRequested(file);
	else if (chosen == highlightAction) {
		if (highlightAction->isChecked())
			_highlighted.insert(file);
		else
			_highlighted.remove(file);

		rebuild();
		emit highlightChanged(_highlighted);
	} else if (chosen == clearHighlightsAction) {
		_highlighted.clear();

		rebuild();
		emit highlightChanged(_highlighted);
	}
}
