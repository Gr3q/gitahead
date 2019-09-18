//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#include "CommitList.h"
#include "Badge.h"
#include "Location.h"
#include "MainWindow.h"
#include "ProgressIndicator.h"
#include "RepoView.h"
#include "app/Application.h"
#include "dialogs/MergeDialog.h"
#include "index/Index.h"
#include "git/Branch.h"
#include "git/Commit.h"
#include "git/Config.h"
#include "git/Diff.h"
#include "git/Index.h"
#include "git/Patch.h"
#include "git/RevWalk.h"
#include "git/Signature.h"
#include "git/TagRef.h"
#include "git/Tree.h"
#include <QAbstractListModel>
#include <QApplication>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QTextLayout>
#include <QtConcurrent>

#if defined(Q_OS_MAC)
#define FONT_SIZE 13
#elif defined(Q_OS_WIN)
#define FONT_SIZE 9
#else
#define FONT_SIZE 10
#endif

namespace {
const bool compactMode = true;

const int kStarPadding = (compactMode) ? 7 : 8;
const int kLineSpacing = (compactMode) ? 23 : 16;
const int kVerticalMargin = (compactMode) ? 5 : 2;
const int kHorizontalMargin = 4;

// FIXME: Factor out into theme?
const QColor kTaintedColor = Qt::gray;

const QString kPathspecFmt = "pathspec:%1";

enum Role
{
  DiffRole = Qt::UserRole,
  CommitRole,
  GraphRole,
  GraphColorRole
};

enum GraphSegment
{
  Dot,
  Top,
  Middle,
  Bottom,
  Cross,
  LeftIn,
  LeftOut,
  RightIn,
  RightOut
};

class DiffCallbacks : public git::Diff::Callbacks
{
public:
  void setCanceled(bool canceled)
  {
    mCanceled = canceled;
  }

  bool progress(const QString &oldPath, const QString &newPath) override
  {
    return !mCanceled;
  }

private:
  bool mCanceled = false;
};

class CommitModel : public QAbstractListModel
{
  Q_OBJECT

public:
  CommitModel(const git::Repository &repo, QObject *parent = nullptr)
    : QAbstractListModel(parent), mRepo(repo)
  {
    // Connect progress timer.
    connect(&mTimer, &QTimer::timeout, [this] {
      ++mProgress;
      QModelIndex idx = index(0, 0);
      emit dataChanged(idx, idx, {Qt::DisplayRole});
    });

    // Connect watcher to signal when the status diff finishes.
    connect(&mStatus, &QFutureWatcher<git::Diff>::finished, [this] {
      mTimer.stop();
      resetWalker();
      emit statusFinished(!mRows.isEmpty() && !mRows.first().commit.isValid());
    });

    git::RepositoryNotifier *notifier = repo.notifier();
    connect(notifier, &git::RepositoryNotifier::referenceUpdated,
            this, &CommitModel::resetReference);
    connect(notifier, &git::RepositoryNotifier::workdirChanged, [this] {
      resetReference(mRef);
    });

    resetSettings();
  }

  git::Reference reference() const
  {
    return mRef;
  }

  git::Diff status() const
  {
    if (!mStatus.isFinished())
      return git::Diff();

    QFuture<git::Diff> future = mStatus.future();
    if (!future.resultCount())
      return git::Diff();

    return future.result();
  }

  void startStatus()
  {
    // Cancel existing status diff.
    cancelStatus();

    // Reload the index before starting the status thread. Allowing
    // it to reload on the thread frequently corrupts the index.
    mRepo.index().read();

    // Check for uncommitted changes asynchronously.
    mProgress = 0;
    mTimer.start(50);
    mStatus.setFuture(QtConcurrent::run([this] {
      // Pass the repo's index to suppress reload.
      return mRepo.status(mRepo.index(), &mStatusCallbacks);
    }));
  }

  void cancelStatus()
  {
    if (!mStatus.isRunning())
      return;

    mStatusCallbacks.setCanceled(true);
    mStatus.waitForFinished();
    mStatus.setFuture(QFuture<git::Diff>());
    mStatusCallbacks.setCanceled(false);
  }

  void setPathspec(const QString &pathspec)
  {
    if (mPathspec == pathspec)
      return;

    mPathspec = pathspec;
    resetWalker();
  }

  void setReference(const git::Reference &ref)
  {
    mRef = ref;
    resetWalker();
  }

  void resetReference(const git::Reference &ref)
  {
    // Reset selected ref to updated ref.
    if (ref.isValid() && mRef.isValid() &&
        ref.qualifiedName() == mRef.qualifiedName())
      mRef = ref;

    // Status is invalid after HEAD changes.
    if (!ref.isValid() || ref.isHead())
      startStatus();

    resetWalker();
  }

  void resetWalker()
  {
    beginResetModel();

    // Reset state.
    mParents.clear();
    mRows.clear();

    // Update status row.
    bool head = (!mRef.isValid() || mRef.isHead());
    bool valid = (mCleanStatus || !mStatus.isFinished() || status().isValid());
    if (head && valid && mPathspec.isEmpty()) {
      QVector<Column> row;
      if (mGraphVisible && mRef.isValid() && mStatus.isFinished()) {
        row.append({Segment(Bottom, kTaintedColor), Segment(Dot, QColor())});
        mParents.append(Parent(mRef.target(), nextColor(), true));
      }

      mRows.append(Row(git::Commit(), row));
    }

    // Begin walking commits.
    if (mRef.isValid()) {
      int sort = GIT_SORT_NONE;
      if (mGraphVisible) {
        sort |= GIT_SORT_TOPOLOGICAL;
        if (mSortDate)
          sort |= GIT_SORT_TIME;
      } else if (!mSortDate) {
        sort |= GIT_SORT_TOPOLOGICAL;
      }

      mWalker = mRef.walker(sort);
      if (mRef.isLocalBranch()) {
        // Add the upstream branch.
        if (git::Branch upstream = git::Branch(mRef).upstream())
          mWalker.push(upstream);
      }

      if (mRef.isHead()) {
        // Add merge head.
        if (git::Reference mergeHead = mRepo.lookupRef("MERGE_HEAD"))
          mWalker.push(mergeHead);
      }

      if (mRefsAll) {
        foreach (const git::Reference ref, mRepo.refs()) {
          if (!ref.isStash())
            mWalker.push(ref);
        }
      }
    }

    if (canFetchMore(QModelIndex()))
      fetchMore(QModelIndex());

    endResetModel();
  }

  void resetSettings(bool walk = false)
  {
    git::Config config = mRepo.appConfig();
    mRefsAll = config.value<bool>("commit.refs.all", true);
    mSortDate = config.value<bool>("commit.sort.date", true);
    mCleanStatus = config.value<bool>("commit.status.clean", false);
    mGraphVisible = config.value<bool>("commit.graph.visible", true);
    mCompactMode = config.value<bool>("commit.compact", false);

    if (walk)
      resetWalker();
  }

  bool canFetchMore(const QModelIndex &parent) const
  {
    return mWalker.isValid();
  }

  void fetchMore(const QModelIndex &parent)
  {
    // Load commits.
    int i = 0;
    QList<Row> rows;
    git::Commit commit = mWalker.next(mPathspec);
    while (commit.isValid()) {
      // Add root commits.
      bool root = false;
      if (indexOf(commit) < 0) {
        root = true;
        mParents.append(Parent(commit, nextColor()));
      }

      // Calculate graph columns.
      // Remember current row.
      QList<Parent> parents = mParents;

      // Replace commit with its parents.
      QList<git::Commit> replacements;
      foreach (const git::Commit &parent, commit.parents()) {
        // FIXME: Mark commits that point to existing parent?
        if (indexOf(parent) < 0 && !contains(parent, rows))
          replacements.append(parent);
      }

      // Set parents for next row.
      int index = indexOf(commit);
      if (index >= 0) {
        Parent parent = mParents.takeAt(index);
        if (!replacements.isEmpty()) {
          git::Commit replacement = replacements.takeFirst();
          mParents.insert(index, Parent(replacement, parent.color));
          foreach (const git::Commit &replacement, replacements)
            mParents.append(Parent(replacement, nextColor()));
        }
      }

      // Add graph row.
      QVector<Column> row;
      if (mGraphVisible && mPathspec.isEmpty())
        row = columns(commit, parents, root);

      rows.append(Row(commit, row));

      // Bail out.
      if (i++ >= 64)
        break;

      commit = mWalker.next(mPathspec);
    }

    // Update the model.
    if (!rows.isEmpty()) {
      int first = mRows.size();
      int last = first + rows.size() - 1;
      beginInsertRows(QModelIndex(), first, last);
      mRows.append(rows);
      endInsertRows();
    }

    // Invalidate walker.
    if (!commit.isValid())
      mWalker = git::RevWalk();
  }

  int rowCount(const QModelIndex &parent = QModelIndex()) const
  {
    return mRows.size();
  }

  QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const
  {
    const Row &row = mRows.at(index.row());
    bool status = !row.commit.isValid();
    switch (role) {
      case Qt::DisplayRole:
        if (!status)
          return QVariant();

        return mStatus.isFinished() ? tr("Uncommitted changes") :
               tr("Checking for uncommitted changes");

      case Qt::FontRole: {
        if (!status)
          return QVariant();

        QFont font = static_cast<QWidget *>(QObject::parent())->font();
        font.setItalic(true);
        return font;
      }

      case Qt::TextAlignmentRole:
        if (!status)
          return QVariant();

        return QVariant(Qt::AlignHCenter | Qt::AlignVCenter);

      case Qt::DecorationRole:
        if (!status)
          return QVariant();

        return mStatus.isFinished() ? QVariant() : mProgress;

      case DiffRole: {
        if (status)
          return QVariant::fromValue(this->status());

        git::Diff diff = row.commit.diff();
        diff.findSimilar();
        return QVariant::fromValue(diff);
      }

      case CommitRole:
        return status ? QVariant() : QVariant::fromValue(row.commit);

      case GraphRole: {
        QVariantList columns;
        foreach (const Column &column, row.columns) {
          QVariantList segments;
          foreach (const Segment &segment, column)
            segments.append(segment.segment);
          columns.append(QVariant(segments));
        }

        return columns;
      }

      case GraphColorRole: {
        QVariantList columns;
        foreach (const Column &column, row.columns) {
          QVariantList segments;
          foreach (const Segment &segment, column)
            segments.append(segment.color);
          columns.append(QVariant(segments));
        }

        return columns;
      }
    }

    return QVariant();
  }

signals:
  void statusFinished(bool visible);

private:
  struct Parent
  {
    Parent(
      const git::Commit &commit,
      const QColor &color,
      bool tainted = false)
      : commit(commit), color(color), tainted(tainted)
    {}

    QColor taintedColor(const git::Commit &commit = git::Commit()) const
    {
      return (tainted && this->commit != commit) ? kTaintedColor : color;
    }

    git::Commit commit;
    QColor color;
    bool tainted;
  };

  struct Segment
  {
    Segment(GraphSegment segment, QColor color)
      : segment(segment), color(color)
    {}

    GraphSegment segment;
    QColor color;
  };

  using Column = QList<Segment>;

  struct Row
  {
    Row(const git::Commit &commit, const QVector<Column> &columns)
      : commit(commit), columns(columns)
    {}

    git::Commit commit;
    QVector<Column> columns;
  };

  int indexOf(const git::Commit &commit) const
  {
    int count = mParents.size();
    for (int i = 0; i < count; ++i) {
      if (mParents.at(i).commit == commit)
        return i;
    }

    return -1;
  }

  bool contains(const git::Commit &commit, const QList<Row> &rows) const
  {
    foreach (const Row &row, mRows) {
      if (row.commit == commit)
        return true;
    }

    foreach (const Row &row, rows) {
      if (row.commit == commit)
        return true;
    }

    return false;
  }

  // The commit and parents parameters represent the current row.
  // The mParents member represents the next row after this one.
  QVector<Column> columns(
    const git::Commit &commit,
    const QList<Parent> &parents,
    bool root)
  {
    int count = parents.size();
    QVector<Column> columns(count);

    // Add incoming paths.
    int incoming = root ? count - 1 : count;
    for (int i = 0; i < incoming; ++i)
      columns[i] << Segment(Top, parents.at(i).taintedColor());

    // Add outgoing paths.
    for (int i = 0; i < count; ++i) {
      // Get the successors of this column.
      QList<git::Commit> successors;
      const Parent &parent = parents.at(i);
      if (parent.commit == commit) {
        successors = parent.commit.parents();
      } else {
        successors.append(parent.commit);
      }

      // Add a path to each successor.
      foreach (const git::Commit &successor, successors) {
        // Find index of parent in next row.
        int index = indexOf(successor);
        if (index < 0)
          continue;

        // Handle multiple commits that share the same parent.
        bool single = (successors.size() == 1);
        const QColor &color =
          single ? parent.taintedColor(commit) : mParents.at(index).color;

        if (index < i) {
          // out to the left
          columns[index] << Segment(RightIn, color);
          for (int j = index + 1; j < i; ++j)
            columns[j] << Segment(Cross, color);
          columns[i] << Segment(LeftOut, color);

        } else if (index > i) {
          // out to the right
          columns[i] << Segment(RightOut, color);
          for (int j = i + 1; j < index; ++j)
            columns[j] << Segment(Cross, color);
          if (index == columns.size())
            columns.append(Column());
          columns[index] << Segment(LeftIn, color);

        } else { // index == i
          // out the bottom
          columns[index] << Segment(Bottom, color);
        }
      }
    }

    // Add middle section last.
    for (int i = 0; i < count; ++i) {
      const Parent &parent = parents.at(i);
      bool dot = (parent.commit == commit);
      columns[i] << Segment(dot ? Dot : Middle, parent.taintedColor());
    }

    return columns;
  }

  QColor nextColor()
  {
    // Get the first unused (or least used) color.
    QMap<QString,int> counts;
    foreach (const Parent &parent, mParents)
      counts[parent.color.name()]++;

    int count = 0;
    QList<QColor> colors = Application::theme()->branchTopologyEdges();
    forever {
      foreach (const QColor &color, colors) {
        if (counts.value(color.name()) == count)
          return color;
      }

      ++count;
    }

    Q_UNREACHABLE();
    return QColor();
  }

  QTimer mTimer;
  int mProgress = 0;

  DiffCallbacks mStatusCallbacks;
  QFutureWatcher<git::Diff> mStatus;

  QString mPathspec;
  git::Reference mRef;
  git::RevWalk mWalker;
  git::Repository mRepo;

  QList<Row> mRows;
  QList<Parent> mParents;

  // walker settings
  bool mRefsAll = true;
  bool mSortDate = true;
  bool mCleanStatus = true;
  bool mGraphVisible = true;
  bool mCompactMode = false; // Needs to be true?
};

class ListModel : public QAbstractListModel
{
public:
  ListModel(QObject *parent = nullptr)
    : QAbstractListModel(parent)
  {}

  void setList(const QList<git::Commit> &commits)
  {
    beginResetModel();
    mCommits = commits;
    endResetModel();
  }

  int rowCount(const QModelIndex &parent = QModelIndex()) const override
  {
    return mCommits.size();
  }

  QVariant data(
    const QModelIndex &index,
    int role = Qt::DisplayRole) const override
  {
    switch (role) {
      case DiffRole: {
        git::Diff diff = mCommits.at(index.row()).diff();
        diff.findSimilar();
        return QVariant::fromValue(diff);
      }

      case CommitRole:
        return QVariant::fromValue(mCommits.at(index.row()));
    }

    return QVariant();
  }

private:
  QList<git::Commit> mCommits;
};

class CommitDelegate : public QStyledItemDelegate
{
public:
  CommitDelegate(const git::Repository &repo, QObject *parent = nullptr)
    : QStyledItemDelegate(parent), mRepo(repo)
  {
    updateRefs();

    git::RepositoryNotifier *notifier = repo.notifier();
    connect(notifier, &git::RepositoryNotifier::referenceUpdated,
            this, &CommitDelegate::updateRefs);
    connect(notifier, &git::RepositoryNotifier::referenceAdded,
            this, &CommitDelegate::updateRefs);
    connect(notifier, &git::RepositoryNotifier::referenceRemoved,
            this, &CommitDelegate::updateRefs);
  }

  void paint(
    QPainter *painter,
    const QStyleOptionViewItem &option,
    const QModelIndex &index) const override
  {
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);

    // Draw background.
    QStyledItemDelegate::paint(painter, opt, index);

    bool active = (opt.state & QStyle::State_Active);
    bool selected = (opt.state & QStyle::State_Selected);
    auto group = active ? QPalette::Active : QPalette::Inactive;
    auto textRole = selected ? QPalette::HighlightedText : QPalette::Text;
    auto brightRole = selected ? QPalette::WindowText : QPalette::BrightText;
    QPalette palette = Application::theme()->commitList();
    QColor text = palette.color(group, textRole);
    QColor bright = palette.color(group, brightRole);

    painter->save();
    painter->setRenderHints(QPainter::Antialiasing);

    // Draw busy indicator.
    if (opt.features & QStyleOptionViewItem::HasDecoration) {
      QRect rect = decorationRect(option, index);
      int progress = index.data(Qt::DecorationRole).toInt();
      ProgressIndicator::paint(painter, rect, bright, progress, opt.widget);
    }

    // Set default foreground color.
    painter->setPen(text);

    // Use default pen color for dot.
    QPen dot = painter->pen();
    dot.setWidth(2);

    // Copy content rect.
    QRect rect = opt.rect;
    rect.setX(rect.x() + 2);

    int totalWidth = rect.width();

    // Draw graph.
    painter->save();
    QVariantList columns = index.data(GraphRole).toList();
    QVariantList colorColumns = index.data(GraphColorRole).toList();
    for (int i = 0; i < columns.size(); ++i) {
      int x = rect.x();
      int y = rect.y();
      int w = opt.fontMetrics.ascent();
      int h = opt.rect.height();
      int h_2 = h / 2;
      int h_4 = h / 4;

      // radius
      int radius = w / 3;

      // xs
      int x1 = x + (w / 2);
      int xr = x + (radius * 2) + 4;
      int x2 = x + w;

      // ys
      int y1 = y + h_2 - radius;
      int y2 = y + h_2;
      int y3 = y + h_2 + radius;
      int y4 = y + h_2 + h_4;
      int y5 = y + h;

      QVariantList segments = columns.at(i).toList();
      QVariantList colors = colorColumns.at(i).toList();
      for (int j = 0; j < segments.size(); ++j) {
        QColor color = colors.at(j).value<QColor>();
        QPen pen(color, 2);
        if (color == kTaintedColor) {
          pen.setStyle(Qt::DashLine);
          pen.setDashPattern({2, 2});
        }

        painter->setPen(pen);
        switch (segments.at(j).toInt()) {
          case Dot:
            painter->setPen(dot);
            painter->drawEllipse(QPoint(x1, y2), radius, radius);
            break;

          case Top:
            painter->drawLine(x1, y, x1, y1);
            break;

          case Middle:
            painter->drawLine(x1, y1, x1, y3);
            break;

          case Bottom:
            painter->drawLine(x1, y3, x1, y5);
            break;

          case Cross:
            painter->drawLine(x, y2, x2, y2);
            break;

          case RightOut: {
            QPainterPath path;
            path.moveTo(xr, y2);
            path.cubicTo(xr, y2, xr, y2, x2, y2);
            painter->drawPath(path);
            break;
          }

          case LeftOut: {
            QPainterPath path;
            path.moveTo(x1, y3);
            path.quadTo(x1, y5-1, x, y5-1);
            painter->drawPath(path);
            break;
          }

          case RightIn: {
            QPainterPath path;
            path.moveTo(x1, y5-1);
            path.quadTo(x1, y5-1, x2, y5-1);
            painter->drawPath(path);
            break;
          }

          case LeftIn: {
            QPainterPath path;
            path.moveTo(x1, y5);
            path.quadTo(x1, y2, x, y2);
            painter->drawPath(path);
            break;
          }
        }
      }

      rect.setX(x + w);

      // Finish early if the graph exceeds one third of the available space.
      if (rect.x() > opt.rect.width() / 3)
        break;
    }

    painter->restore();

    // Adjust margins.
    rect.setY(rect.y() + kVerticalMargin);
    rect.setX(rect.x() + kHorizontalMargin);
    if (!compactMode) rect.setWidth(rect.width() - kHorizontalMargin); // Star has enough padding in compact mode

    // Draw content.
    git::Commit commit = index.data(CommitRole).value<git::Commit>();
    if (commit.isValid()) {
      const QFontMetrics &fm = opt.fontMetrics;
      QRect star = rect;

      if (compactMode) {
        int maxWidthRefs = (int)(rect.width() * 0.5); // Max 30% of the 
        const int minWidthRefs = 50; // At least display The ellipsis
        const int minWidthRequestDesc = 100;
        int minDisplayWidthDate = 350;
        int minDisplayWidthName = 800;
        int minWidthName = 50;
        int maxWidthName = (int)(rect.width() * 0.15);

        // Star always takes up its height on the right side
        star.setX(star.x() + star.width() - star.height());
        star.setY(star.y() - kVerticalMargin);

        // maybe..maybe?? not needed in compact mode?
        // can't really remove it, at least it 
        // needs to be an option
        QString id = commit.shortId();
        QRect box = rect;
        box.setWidth(box.width() - star.width());
        // Using the biggest theoretical width
        int idWidth = fm.horizontalAdvance("9999999") + kHorizontalMargin;
        painter->save();
        painter->drawText(box, Qt::AlignRight, id);
        painter->restore();
        box.setWidth(box.width() - idWidth);

        // Draw date. Only if String is not the same as previous?
        QDateTime date = commit.committer().date().toLocalTime();
        QString timestamp =
          (date.date() == QDate::currentDate()) ?
          date.time().toString(Qt::DefaultLocaleShortDate) :
          date.date().toString(Qt::DefaultLocaleShortDate);
        if (box.width() > minWidthRequestDesc + fm.horizontalAdvance(timestamp) + 8 && totalWidth > minDisplayWidthDate) {
          painter->save();
          painter->setPen(bright);
          painter->drawText(box, Qt::AlignRight, timestamp);
          painter->restore();
          box.setWidth(box.width() - fm.horizontalAdvance(timestamp) - kHorizontalMargin);
        }

        // Name is not needed, does not give context or reference
        /*if (totalWidth > minDisplayWidthName) {
          QString name = commit.author().name();
          painter->save();
          painter->drawText(box, Qt::AlignRight, name);
          painter->restore();
          box.setWidth(box.width() - fm.width(name) - kHorizontalMargin);
        }*/

        QRect ref = box;
        // calculate remaining width for the references
        int refsWidth = ref.width() - minWidthRequestDesc;
        if (maxWidthRefs <= minWidthRefs) maxWidthRefs = minWidthRefs;
        if (refsWidth < minWidthRefs) refsWidth = minWidthRefs;
        if (refsWidth > maxWidthRefs) refsWidth = maxWidthRefs;
        ref.setWidth(refsWidth);

        // Draw references.
        int badgesWidth = rect.x();
        QList<Badge::Label> refs = mRefs.value(commit.id());
        if (!refs.isEmpty()) {
          badgesWidth = Badge::paint(painter, refs, ref, &opt, LEFT);
        }

        box.setX(badgesWidth); // Comes right after the badges

        // Draw message.
        painter->save();
        painter->setPen(bright);
        QString msg = commit.summary(git::Commit::SubstituteEmoji);
        QString elidedText = fm.elidedText(msg, Qt::ElideRight, box.width());
        painter->drawText(box, Qt::ElideRight, elidedText);
        painter->restore();
      }

      if (!compactMode) {
        // Draw Name.
        QString name = commit.author().name();
        painter->save();
        QFont bold = opt.font;
        bold.setBold(true);
        painter->setFont(bold);
        painter->drawText(rect, Qt::AlignLeft, name);
        painter->restore();

        // Draw date.
        QDateTime date = commit.committer().date().toLocalTime();
        QString timestamp =
          (date.date() == QDate::currentDate()) ?
          date.time().toString(Qt::DefaultLocaleShortDate) :
          date.date().toString(Qt::DefaultLocaleShortDate);
        if (rect.width() > fm.width(name) + fm.width(timestamp) + 8) {
          painter->save();
          painter->setPen(bright);
          painter->drawText(rect, Qt::AlignRight, timestamp);
          painter->restore();
        }

        rect.setY(rect.y() + kLineSpacing + kVerticalMargin);

        // Draw id.
        QString id = commit.shortId();
        painter->save();
        painter->drawText(rect, Qt::AlignLeft, id);
        painter->restore();

        // Draw references.
        QList<Badge::Label> refs = mRefs.value(commit.id());
        if (!refs.isEmpty()) {
          QRect refsRect = rect;
          refsRect.setX(refsRect.x() + fm.boundingRect(id).width() + 6);
          Badge::paint(painter, refs, refsRect, &opt);
        }

        rect.setY(rect.y() + kLineSpacing + kVerticalMargin);

        // Divide remaining rectangle.
        star = rect;
        star.setX(star.x() + star.width() - star.height());
        QRect text = rect;
        text.setWidth(text.width() - star.width());

        // Draw message.
        painter->save();
        painter->setPen(bright);
        QString msg = commit.summary(git::Commit::SubstituteEmoji);
        QTextLayout layout(msg, painter->font());
        layout.beginLayout();

        QTextLine line = layout.createLine();
        if (line.isValid()) {
          int width = text.width();
          line.setLineWidth(width);
          int len = line.textLength();
          painter->drawText(text, Qt::AlignLeft, msg.left(len));

          if (len < msg.length()) {
            text.setY(text.y() + kLineSpacing);
            QString elided = fm.elidedText(msg.mid(len), Qt::ElideRight, width);
            painter->drawText(text, Qt::AlignLeft, elided);
          }
        }

        layout.endLayout();
        painter->restore();
      }

      // Draw star.
      bool starred = commit.isStarred();
      const QAbstractItemView *view =
        static_cast<const QAbstractItemView *>(opt.widget);
      QPoint pos = view->viewport()->mapFromGlobal(QCursor::pos());
      if (starred || (view->underMouse() && view->indexAt(pos) == index)) {
        painter->save();

        // Calculate outer radius and vertices.
        qreal radius = (star.height() / 2.0) - kStarPadding;
        qreal x = star.x() + (star.width() / 2.0);
        qreal y = star.y() + (star.height() / 2.0);
        qreal x1 = radius * qCos(M_PI / 10.0);
        qreal y1 = -radius * qSin(M_PI / 10.0);
        qreal x2 = radius * qCos(17.0 * M_PI / 10.0);
        qreal y2 = -radius * qSin(17.0 * M_PI / 10.0);

        // Calculate inner radius and verices.
        qreal xi = ((y1 + radius) * x2) / (y2 + radius);
        qreal ri = qSqrt(qPow(xi, 2.0) + qPow(y1, 2.0));
        qreal xi1 = ri * qCos(3.0 * M_PI / 10.0);
        qreal yi1 = -ri * qSin(3.0 * M_PI / 10.0);
        qreal xi2 = ri * qCos(19.0 * M_PI / 10.0);
        qreal yi2 = -ri * qSin(19.0 * M_PI / 10.0);

        QPolygonF polygon({
          QPointF(0, -radius),
          QPointF(xi1, yi1),
          QPointF(x1, y1),
          QPointF(xi2, yi2),
          QPointF(x2, y2),
          QPointF(0, ri),
          QPointF(-x2, y2),
          QPointF(-xi2, yi2),
          QPointF(-x1, y1),
          QPointF(-xi1, yi1)
        });

        if (starred)
          painter->setBrush(Application::theme()->star());

        painter->setPen(QPen(bright, 1.25));
        painter->drawPolygon(polygon.translated(x, y));
        painter->restore();
      }
    }

    // Is the next index selected?
    bool nextSelected = false;

#ifndef Q_OS_WIN
    // Draw separator between selected indexes.
    QModelIndex next = index.sibling(index.row() + 1, 0);
    if (next.isValid()) {
      const QAbstractItemView *view =
        static_cast<const QAbstractItemView *>(opt.widget);
      nextSelected = view->selectionModel()->isSelected(next);
    }
#endif
    

    // Draw separator line.
    if (!compactMode && selected == nextSelected) {
      painter->save();
      painter->setRenderHints(QPainter::Antialiasing, false);
      painter->setPen(selected ? text : opt.palette.color(QPalette::Dark));
      painter->drawLine(rect.bottomLeft(), rect.bottomRight());
      painter->restore();
    }
    
    painter->restore();
  }

  QSize sizeHint(
    const QStyleOptionViewItem &option,
    const QModelIndex &index) const override
  {
    int verticalSize = kLineSpacing + kVerticalMargin;
    if (!compactMode) verticalSize = verticalSize * 4;
    return QSize(0, verticalSize);
  }

  QRect decorationRect(
    const QStyleOptionViewItem &option,
    const QModelIndex &index) const
  {
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);

    QStyle *style = opt.widget ? opt.widget->style() : QApplication::style();
    QStyle::SubElement se = QStyle::SE_ItemViewItemDecoration;
    return style->subElementRect(se, &opt, opt.widget);
  }

  QRect starRect(
    const QStyleOptionViewItem &option,
    const QModelIndex &index) const
  {
    QRect rect = option.rect;
    int length = kLineSpacing * 2;
    rect.setX(rect.x() + rect.width() - length);
    rect.setY(rect.y() + rect.height() - length);
    rect.setWidth(rect.width() - kStarPadding);
    rect.setHeight(rect.height() - kStarPadding);
    return rect;
  }

protected:
  void initStyleOption(
    QStyleOptionViewItem *option,
    const QModelIndex &index) const override
  {
    QStyledItemDelegate::initStyleOption(option, index);
    if (index.data(Qt::DecorationRole).canConvert<int>())
      option->decorationSize = ProgressIndicator::size();
  }

private:
  void updateRefs()
  {
    mRefs.clear();

    if (mRepo.isHeadDetached()) {
      git::Reference head = mRepo.head();
      mRefs[head.target().id()].append({head.name(), true});
    }

    foreach (const git::Reference &ref, mRepo.refs()) {
      if (git::Commit target = ref.target())
        mRefs[target.id()].append({ref.name(), ref.isHead(), ref.isTag()});
    }
  }

  git::Repository mRepo;
  QMap<git::Id,QList<Badge::Label>> mRefs;
};

class SelectionModel : public QItemSelectionModel
{
public:
  SelectionModel(QAbstractItemModel *model)
    : QItemSelectionModel(model)
  {}

  void select(
    const QItemSelection &selection,
    QItemSelectionModel::SelectionFlags command)
  {
    if ((command == QItemSelectionModel::Select ||
         command == QItemSelectionModel::SelectCurrent ||
         command == (QItemSelectionModel::Current |
                     QItemSelectionModel::ClearAndSelect)) &&
        (selectedIndexes().size() >= 2 || selection.indexes().size() > 1))
          return;

    QItemSelectionModel::select(selection, command);
  }
};

} // anon. namespace

CommitList::CommitList(Index *index, QWidget *parent)
  : QListView(parent), mIndex(index)
{
  Theme *theme = Application::theme();
  setPalette(theme->commitList());

  git::Repository repo = index->repo();
  mList = new ListModel(this);
  mModel = new CommitModel(repo, this);

  setMouseTracking(true);
  setUniformItemSizes(true);
  setAttribute(Qt::WA_MacShowFocusRect, false);
  setSelectionMode(QAbstractItemView::ExtendedSelection);

  setModel(mModel);
  setItemDelegate(new CommitDelegate(repo, this));

  connect(mModel, &QAbstractItemModel::modelAboutToBeReset,
          this, &CommitList::storeSelection);
  connect(mModel, &QAbstractItemModel::modelReset,
          this, &CommitList::restoreSelection);
  connect(mList, &QAbstractItemModel::modelAboutToBeReset,
          this, &CommitList::storeSelection);
  connect(mList, &QAbstractItemModel::modelReset,
          this, &CommitList::restoreSelection);

  CommitModel *model = static_cast<CommitModel *>(mModel);
  connect(model, &CommitModel::statusFinished, [this](bool visible) {
    // Fake a selection notification if the diff is visible and selected.
    if (visible && selectionModel()->isSelected(mModel->index(0, 0)))
      resetSelection();

    // Select the first commit if the selection was cleared.
    if (selectedIndexes().isEmpty())
      selectFirstCommit();

    // Notify main window.
    emit statusChanged(visible);
  });

  connect(this, &CommitList::entered, [this](const QModelIndex &index) {
    update(index);
  });

  git::RepositoryNotifier *notifier = repo.notifier();
  connect(notifier, &git::RepositoryNotifier::referenceUpdated,
  [this](const git::Reference &ref) {
    if (!ref.isValid())
      return;

    if (ref.isStash())
      updateModel();

    if (ref.isHead()) {
      QModelIndex index = this->model()->index(0, 0);
      if (!index.data(CommitRole).isValid()) {
        selectFirstCommit();
      } else {
        selectRange(ref.target().id().toString());
      }
    }
  });

  QFont font = this->font();
  font.setPointSize(FONT_SIZE);
  setFont(font);
}

git::Diff CommitList::status() const
{
  return static_cast<CommitModel *>(mModel)->status();
}

QString CommitList::selectedRange() const
{
  QList<git::Commit> commits = selectedCommits();
  if (commits.isEmpty())
    return !selectedIndexes().isEmpty() ? "status" : QString();

  git::Commit first = commits.first();
  if (commits.size() == 1)
    return first.id().toString();

  git::Commit last = commits.last();
  return QString("%1..%2").arg(last.id().toString(), first.id().toString());
}

git::Diff CommitList::selectedDiff() const
{
  QModelIndexList indexes = sortedIndexes();
  if (indexes.isEmpty())
    return git::Diff();

  if (indexes.size() == 1)
    return indexes.first().data(DiffRole).value<git::Diff>();

  git::Commit first = indexes.first().data(CommitRole).value<git::Commit>();
  if (!first.isValid())
    return git::Diff();

  git::Commit last = indexes.last().data(CommitRole).value<git::Commit>();
  git::Diff diff = first.diff(last);
  diff.findSimilar();
  return diff;
}

QList<git::Commit> CommitList::selectedCommits() const
{
  QList<git::Commit> selectedCommits;
  foreach (const QModelIndex &index, sortedIndexes()) {
    git::Commit commit = index.data(CommitRole).value<git::Commit>();
    if (commit.isValid())
      selectedCommits.append(commit);
  }

  return selectedCommits;
}

void CommitList::cancelStatus()
{
  static_cast<CommitModel *>(mModel)->cancelStatus();
}

void CommitList::setReference(const git::Reference &ref)
{
  static_cast<CommitModel *>(mModel)->setReference(ref);
  updateModel();
  setFocus();
}

void CommitList::setFilter(const QString &filter)
{
  mFilter = filter.simplified();
  updateModel();
}

void CommitList::setPathspec(const QString &pathspec, bool index)
{
  if (index) {
    setFilter(!pathspec.isEmpty() ? kPathspecFmt.arg(pathspec) : QString());
  } else {
    static_cast<CommitModel *>(mModel)->setPathspec(pathspec);
  }
}

void CommitList::setCommits(const QList<git::Commit> &commits)
{
  setModel(mList);
  static_cast<ListModel *>(mList)->setList(commits);
}

void CommitList::selectReference(const git::Reference &ref)
{
  if (!ref.isValid())
    return;

  QModelIndex index = model()->index(0, 0);
  if (ref.isHead() && !index.data(CommitRole).isValid()) {
    selectFirstCommit();
  } else {
    selectRange(ref.target().id().toString());
  }
}

void CommitList::resetSelection(bool spontaneous)
{
  // Just notify.
  mSpontaneous = spontaneous;
  notifySelectionChanged();
  mSpontaneous = true;
}

void CommitList::selectFirstCommit(bool spontaneous)
{
  QModelIndex index = model()->index(0, 0);
  if (index.isValid()) {
    selectIndexes(QItemSelection(index, index), QString(), spontaneous);
  } else {
    emit diffSelected(git::Diff());
  }
}

bool CommitList::selectRange(
  const QString &range,
  const QString &file,
  bool spontaneous)
{
  // Try to select the "status" index.
  QModelIndex index = model()->index(0, 0);
  if (range == "status" && !index.data(CommitRole).isValid()) {
    selectFirstCommit();
    return true;
  }

  QStringList ids = range.split("..");
  if (ids.size() > 2)
    return false;

  // Invert range.
  bool one = (ids.size() == 1);
  git::Repository repo = RepoView::parentView(this)->repo();
  git::Commit firstCommit = repo.lookupCommit(ids.last());
  git::Commit lastCommit = one ? firstCommit : repo.lookupCommit(ids.first());

  // Check for already selected range.
  QModelIndexList indexes = sortedIndexes();
  if (indexes.size() >= 2) {
    git::Commit first = indexes.first().data(CommitRole).value<git::Commit>();
    git::Commit last = indexes.last().data(CommitRole).value<git::Commit>();
    if (first.isValid() && first == firstCommit &&
        last.isValid() && last == lastCommit)
      return false;
  }

  // Find indexes.
  QItemSelection selection;
  QModelIndex first = findCommit(firstCommit);
  if (!first.isValid())
    return false;
  selection.select(first, first);

  if (lastCommit != firstCommit) {
    QModelIndex last = findCommit(lastCommit);
    if (!last.isValid())
      return false;
    selection.select(last, last);
  }

  selectIndexes(selection, file, spontaneous);
  return true;
}

void CommitList::resetSettings()
{
  static_cast<CommitModel *>(mModel)->resetSettings(true);
}

void CommitList::setModel(QAbstractItemModel *model)
{
  if (model == this->model())
    return;

  storeSelection();

  // Destroy the previous selection model.
  delete selectionModel();

  QListView::setModel(model);

  // Destroy the selection model created by Qt.
  delete selectionModel();

  SelectionModel *selectionModel = new SelectionModel(model);
  connect(selectionModel, &QItemSelectionModel::selectionChanged,
  [this](const QItemSelection &selected, const QItemSelection &deselected) {
    // Update the index before each selected/deselected range.
    foreach (const QItemSelectionRange &range, selected + deselected) {
      if (int row = range.top())
        update(this->model()->index(row - 1, 0));
    }

    notifySelectionChanged();
  });

  setSelectionModel(selectionModel);

  restoreSelection();
}

void CommitList::contextMenuEvent(QContextMenuEvent *event)
{
  QModelIndex index = indexAt(event->pos());
  if (!index.isValid())
    return;

  RepoView *view = RepoView::parentView(this);
  git::Commit commit = index.data(CommitRole).value<git::Commit>();

  if (!commit.isValid()) {
    QMenu menu;

    // clean
    QStringList untracked;
    if (git::Diff diff = status()) {
      for (int i = 0; i < diff.count(); i++) {
        if (diff.status(i) == GIT_DELTA_UNTRACKED)
          untracked.append(diff.name(i));
      }
    }

    QAction *clean = menu.addAction(tr("Remove Untracked Files"),
    [view, untracked] {
      view->clean(untracked);
    });

    clean->setEnabled(!untracked.isEmpty());

    menu.exec(event->globalPos());
    return;
  }

  QMenu menu;
  menu.setToolTipsVisible(true);

  // stash
  git::Reference ref = static_cast<CommitModel *>(mModel)->reference();
  if (ref.isValid() && ref.isStash()) {
    menu.addAction(tr("Apply"), [view, index] {
      view->applyStash(index.row());
    });

    menu.addAction(tr("Pop"), [view, index] {
      view->popStash(index.row());
    });

    menu.addAction(tr("Drop"), [view, index] {
      view->dropStash(index.row());
    });

  } else {
    // multiple selection
    bool anyStarred = false;
    foreach (const QModelIndex &index, selectionModel()->selectedIndexes()) {
      if (index.data(CommitRole).value<git::Commit>().isStarred()) {
        anyStarred = true;
        break;
      }
    }

    menu.addAction(anyStarred ? tr("Unstar") : tr("Star"), [this, anyStarred] {
      foreach (const QModelIndex &index, selectionModel()->selectedIndexes())
        index.data(CommitRole).value<git::Commit>().setStarred(!anyStarred);
    });

    // single selection
    if (selectionModel()->selectedIndexes().size() <= 1) {
      menu.addSeparator();

      menu.addAction(tr("Add Tag..."), [view, commit] {
        view->promptToTag(commit);
      });

      menu.addAction(tr("New Branch..."), [view, commit] {
        view->promptToCreateBranch(commit);
      });

      menu.addSeparator();

      menu.addAction(tr("Merge..."), [view, commit] {
        MergeDialog *dialog =
          new MergeDialog(RepoView::Merge, view->repo(), view);
        connect(dialog, &QDialog::accepted, [view, dialog] {
          git::AnnotatedCommit upstream;
          git::Reference ref = dialog->reference();
          if (!ref.isValid())
            upstream = dialog->target().annotatedCommit();
          view->merge(dialog->flags(), ref, upstream);
        });

        dialog->setCommit(commit);
        dialog->open();
      });

      menu.addAction(tr("Rebase..."), [view, commit] {
        MergeDialog *dialog =
          new MergeDialog(RepoView::Rebase, view->repo(), view);
        connect(dialog, &QDialog::accepted, [view, dialog] {
          git::AnnotatedCommit upstream;
          git::Reference ref = dialog->reference();
          if (!ref.isValid())
            upstream = dialog->target().annotatedCommit();
          view->merge(dialog->flags(), ref, upstream);
        });

        dialog->setCommit(commit);
        dialog->open();
      });

      menu.addAction(tr("Revert"), [view, commit] {
        view->revert(commit);
      });

      menu.addAction(tr("Cherry-pick"), [view, commit] {
        view->cherryPick(commit);
      });

      menu.addSeparator();

      git::Reference head = view->repo().head();
      foreach (const git::Reference &ref, commit.refs()) {
        if (ref.isLocalBranch()) {
          QAction *checkout = menu.addAction(tr("Checkout %1").arg(ref.name()),
          [view, ref] {
            view->checkout(ref);
          });

          checkout->setEnabled(
            head.isValid() &&
            head.qualifiedName() != ref.qualifiedName() &&
            !view->repo().isBare());
        } else if (ref.isRemoteBranch()) {
          QAction *checkout = menu.addAction(tr("Checkout %1").arg(ref.name()),
          [view, ref] {
            view->checkout(ref);
          });

          // Calculate local branch name in the same way as checkout() does
          QString local = ref.name().section('/', 1);
          if (!head.isValid()) { // I'm not sure when this can happen
            checkout->setEnabled(false);
          } else if (head.name() == local) {
            checkout->setEnabled(false);
            checkout->setToolTip(tr("Local branch is already checked out"));
          } else if (view->repo().isBare()) {
            checkout->setEnabled(false);
            checkout->setToolTip(tr("This is a bare repository"));
          }
        }
      }

      QString name = commit.detachedHeadName();
      QAction *checkout = menu.addAction(tr("Checkout %1").arg(name),
      [view, commit] {
        view->checkout(commit);
      });

      checkout->setEnabled(head.isValid() &&
                           head.target() != commit &&
                           !view->repo().isBare());

      menu.addSeparator();

      QMenu *reset = menu.addMenu(tr("Reset"));
      reset->addAction(tr("Soft"))->setData(GIT_RESET_SOFT);
      reset->addAction(tr("Mixed"))->setData(GIT_RESET_MIXED);
      reset->addAction(tr("Hard"))->setData(GIT_RESET_HARD);
      connect(reset, &QMenu::triggered, [view, commit](QAction *action) {
        git_reset_t type = static_cast<git_reset_t>(action->data().toInt());
        view->promptToReset(commit, type);
      });

      reset->setEnabled(head.isValid() && head.isLocalBranch());
    }
  }

  menu.exec(event->globalPos());
}

void CommitList::mouseMoveEvent(QMouseEvent *event)
{
  if (mStar.isValid() || mCancel.isValid())
    return;

  QListView::mouseMoveEvent(event);
}

void CommitList::mousePressEvent(QMouseEvent *event)
{
  QPoint pos = event->pos();
  QModelIndex index = indexAt(pos);
  mStar = isStar(index, pos) ? index : QModelIndex();
  mCancel = isDecoration(index, pos) ? index : QModelIndex();

  if (mStar.isValid() || mCancel.isValid())
    return;

  QListView::mousePressEvent(event);
}

void CommitList::mouseReleaseEvent(QMouseEvent *event)
{
  QPoint pos = event->pos();
  QModelIndex index = indexAt(pos);
  if (mStar == index && isStar(index, pos)) {
    if (git::Commit commit = index.data(CommitRole).value<git::Commit>()) {
      commit.setStarred(!commit.isStarred());
      update(index); // FIXME: Add signal?
    }
  } else if (mCancel == index && isDecoration(index, pos)) {
    static_cast<CommitModel *>(model())->cancelStatus();
  }

  mStar = QModelIndex();
  mCancel = QModelIndex();

  QListView::mouseReleaseEvent(event);
}

void CommitList::leaveEvent(QEvent *event)
{
  viewport()->update();
  QListView::leaveEvent(event);
}

void CommitList::storeSelection()
{
  mSelectedRange = selectedRange();
}

void CommitList::restoreSelection()
{
  // Restore selection.
  if (!mSelectedRange.isEmpty() && !selectRange(mSelectedRange))
    emit diffSelected(git::Diff());

  mSelectedRange = QString();
}

void CommitList::updateModel()
{
  if (!mFilter.isEmpty()) {
    setCommits(mIndex->commits(mFilter));
    return;
  }

  git::Reference ref = static_cast<CommitModel *>(mModel)->reference();
  if (ref.isValid() && ref.isStash()) {
    setCommits(ref.repo().stashes());
    return;
  }

  // Reset model.
  setModel(mModel);
}

QModelIndexList CommitList::sortedIndexes() const
{
  QModelIndexList indexes = selectedIndexes();
  std::sort(indexes.begin(), indexes.end(),
  [](const QModelIndex &lhs, const QModelIndex &rhs) {
    return lhs.row() < rhs.row();
  });

  return indexes;
}

QModelIndex CommitList::findCommit(const git::Commit &commit)
{
  // Get the 'uncommitted changes' index.
  QAbstractItemModel *model = this->model();
  if (!commit.isValid()) {
    QModelIndex index = model->index(0, 0);
    git::Commit tmp = index.data(CommitRole).value<git::Commit>();
    return !tmp.isValid() ? index : QModelIndex();
  }

  // Find the id.
  QDateTime date = commit.committer().date();
  for (int i = 0; i < model->rowCount(); ++i) {
    QModelIndex index = model->index(i, 0);
    if (git::Commit tmp = index.data(CommitRole).value<git::Commit>()) {
      if (tmp == commit)
        return index;

      // Cut off search if we find an older commit.
      if (tmp.committer().date() < date)
        return QModelIndex();
    }

    // Load more commits.
    if (i == model->rowCount() - 1 && model->canFetchMore(QModelIndex()))
      model->fetchMore(QModelIndex());
  }

  return QModelIndex();
}

void CommitList::selectIndexes(
  const QItemSelection &selection,
  const QString &file,
  bool spontaneous)
{
  mFile = file;
  mSpontaneous = spontaneous;
  selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
  mSpontaneous = true;
  mFile = QString();

  QModelIndexList indexes = selection.indexes();
  if (!indexes.isEmpty())
    scrollTo(indexes.first());
}

void CommitList::notifySelectionChanged()
{
  // Multiple selection means that the selected parameter
  // could be empty when there are still indexes selected.
  QModelIndexList indexes = selectedIndexes();
  if (indexes.isEmpty())
    return;

  // Redraw all selected indexes. Separators may have changed.
  foreach (const QModelIndex &index, indexes)
    update(index);

  emit diffSelected(selectedDiff(), mFile, mSpontaneous);
}

bool CommitList::isDecoration(const QModelIndex &index, const QPoint &pos)
{
  if (!index.isValid())
    return false;

  CommitDelegate *delegate = static_cast<CommitDelegate *>(itemDelegate());
  QStyleOptionViewItem options = viewOptions();
  options.rect = visualRect(index);
  return delegate->decorationRect(options, index).contains(pos);
}

bool CommitList::isStar(const QModelIndex &index, const QPoint &pos)
{
  if (!index.isValid() || !index.data(CommitRole).isValid())
    return false;

  CommitDelegate *delegate = static_cast<CommitDelegate *>(itemDelegate());
  QStyleOptionViewItem options = viewOptions();
  options.rect = visualRect(index);
  return delegate->starRect(options, index).contains(pos);
}

#include "CommitList.moc"
