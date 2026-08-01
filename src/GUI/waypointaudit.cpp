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
	QList<QByteArray> ids(QTimeZone::availableTimeZoneIds());
	std::sort(ids.begin(), ids.end());
	int current = 0;
	for (int i = 0; i < ids.size(); i++) {
		QString id(QString::fromLatin1(ids.at(i)));
		_timeZoneCombo->addItem(id);
		if (ids.at(i) == _timeZone.id())
			current = i;
	}
	_timeZoneCombo->setCurrentIndex(current);
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

	_timeZone = QTimeZone(_timeZoneCombo->itemText(index).toLatin1());
	rebuild();
}

void WaypointAudit::useSystemTimeZone()
{
	QTimeZone system(QTimeZone::systemTimeZone());
	int idx = _timeZoneCombo->findText(QString::fromLatin1(system.id()));
	if (idx >= 0)
		_timeZoneCombo->setCurrentIndex(idx);
}

void WaypointAudit::showContextMenu(const QPoint &pos)
{
	QTreeWidgetItem *item = _tree->itemAt(pos);
	if (!item)
		return;

	QVariant fv(item->data(0, FILE_ROLE));
	if (!fv.isValid())
		return;
	QString file(fv.toString());

	QMenu menu(_tree);
	QAction *highlightAction = menu.addAction(
	  tr("Highlight %1").arg(QFileInfo(file).fileName()));
	highlightAction->setCheckable(true);
	highlightAction->setChecked(_highlighted.contains(file));
	QAction *closeAction = menu.addAction(
	  tr("Close %1").arg(QFileInfo(file).fileName()));

	QAction *chosen = menu.exec(_tree->viewport()->mapToGlobal(pos));
	if (chosen == closeAction)
		emit fileCloseRequested(file);
	else if (chosen == highlightAction) {
		if (highlightAction->isChecked())
			_highlighted.insert(file);
		else
			_highlighted.remove(file);

		rebuild();
		emit highlightChanged(_highlighted);
	}
}
