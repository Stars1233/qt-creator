// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "cppcodemodelinspectordialog.h"

#include "baseeditordocumentprocessor.h"
#include "cppcodemodelinspectordumper.h"
#include "cppeditortr.h"
#include "cppeditorwidget.h"
#include "cppmodelmanager.h"
#include "cpptoolsreuse.h"
#include "cppworkingcopy.h"
#include "editordocumenthandle.h"

#include <coreplugin/editormanager/editormanager.h>
#include <coreplugin/icore.h>

#include <projectexplorer/projectmacro.h>
#include <projectexplorer/project.h>

#include <cplusplus/CppDocument.h>
#include <cplusplus/Overview.h>
#include <cplusplus/Token.h>

#include <utils/fancylineedit.h>
#include <utils/layoutbuilder.h>
#include <utils/qtcassert.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QTabWidget>
#include <QTreeView>

#include <algorithm>

using namespace Core;
using namespace CPlusPlus;
using namespace Utils;

namespace CMI = CppEditor::CppCodeModelInspector;

namespace {

template <class T> void resizeColumns(QTreeView *view)
{
    for (int column = 0; column < T::ColumnCount - 1; ++column)
        view->resizeColumnToContents(column);
}

FilePath fileInCurrentEditor()
{
    if (auto editor = TextEditor::BaseTextEditor::currentTextEditor())
        return editor->document()->filePath();
    return {};
}

QSizePolicy sizePolicyWithStretchFactor(int stretchFactor)
{
    QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    policy.setHorizontalStretch(stretchFactor);
    return policy;
}

class DepthFinder : public SymbolVisitor
{
public:
    int operator()(const Document::Ptr &document, Symbol *symbol)
    {
        m_symbol = symbol;
        accept(document->globalNamespace());
        return m_foundDepth;
    }

    bool preVisit(Symbol *symbol) override
    {
        if (m_stop)
            return false;

        if (symbol->asScope()) {
            ++m_depth;
            if (symbol == m_symbol) {
                m_foundDepth = m_depth;
                m_stop = true;
            }
            return true;
        }

        return false;
    }

    void postVisit(Symbol *symbol) override
    {
        if (symbol->asScope())
            --m_depth;
    }

private:
    Symbol *m_symbol = nullptr;
    int m_depth = -1;
    int m_foundDepth = -1;
    bool m_stop = false;
};

} // anonymous namespace

namespace CppEditor::Internal {

// --- FilterableView -----------------------------------------------------------------------------

class FilterableView : public QWidget
{
    Q_OBJECT
public:
    FilterableView(QWidget *parent);

    void setModel(QAbstractItemModel *model);
    QItemSelectionModel *selectionModel() const;
    void selectIndex(const QModelIndex &index);
    void resizeColumns(int columnCount);
    void clearFilter();

signals:
    void filterChanged(const QString &filterText);

private:
    QTreeView *view;
    FancyLineEdit *lineEdit;
};

FilterableView::FilterableView(QWidget *parent)
    : QWidget(parent)
{
    view = new QTreeView(this);
    view->setAlternatingRowColors(true);
    view->setTextElideMode(Qt::ElideMiddle);
    view->setSortingEnabled(true);

    lineEdit = new FancyLineEdit(this);
    lineEdit->setFiltering(true);
    lineEdit->setPlaceholderText(QLatin1String("File Path"));
    QObject::connect(lineEdit, &QLineEdit::textChanged, this, &FilterableView::filterChanged);

    QLabel *label = new QLabel(QLatin1String("&Filter:"), this);
    label->setBuddy(lineEdit);

    auto filterBarLayout = new QHBoxLayout();
    filterBarLayout->addWidget(label);
    filterBarLayout->addWidget(lineEdit);

    auto mainLayout = new QVBoxLayout();
    mainLayout->addWidget(view);
    mainLayout->addLayout(filterBarLayout);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    setLayout(mainLayout);
}

void FilterableView::setModel(QAbstractItemModel *model)
{
    view->setModel(model);
}

QItemSelectionModel *FilterableView::selectionModel() const
{
    return view->selectionModel();
}

void FilterableView::selectIndex(const QModelIndex &index)
{
    if (index.isValid())  {
        view->selectionModel()->setCurrentIndex(index,
            QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }
}

void FilterableView::resizeColumns(int columnCount)
{
    for (int column = 0; column < columnCount - 1; ++column)
        view->resizeColumnToContents(column);
}

void FilterableView::clearFilter()
{
    lineEdit->clear();
}

// --- ProjectFilesModel --------------------------------------------------------------------------

class ProjectFilesModel : public QAbstractListModel
{
public:
    ProjectFilesModel() = default;

    void configure(const ProjectFiles &files);
    void clear();

    enum Columns { FileKindColumn, FilePathColumn, ColumnCount };

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    ProjectFiles m_files;
};

void ProjectFilesModel::configure(const ProjectFiles &files)
{
    emit layoutAboutToBeChanged();
    m_files = files;
    emit layoutChanged();
}

void ProjectFilesModel::clear()
{
    emit layoutAboutToBeChanged();
    m_files.clear();
    emit layoutChanged();
}

int ProjectFilesModel::rowCount(const QModelIndex &/*parent*/) const
{
    return m_files.size();
}

int ProjectFilesModel::columnCount(const QModelIndex &/*parent*/) const
{
    return ProjectFilesModel::ColumnCount;
}

QVariant ProjectFilesModel::data(const QModelIndex &index, int role) const
{
    if (role == Qt::DisplayRole) {
        const int row = index.row();
        const int column = index.column();
        if (column == FileKindColumn)
            return CMI::Utils::toString(m_files.at(row).kind);
        if (column == FilePathColumn)
            return m_files.at(row).path.toVariant();
    } else if (role == Qt::ForegroundRole) {
        if (!m_files.at(index.row()).active) {
            return QApplication::palette().color(QPalette::ColorGroup::Disabled,
                                                 QPalette::ColorRole::Text);
        }
    }
    return QVariant();
}

QVariant ProjectFilesModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
        switch (section) {
        case FileKindColumn:
            return QLatin1String("File Kind");
        case FilePathColumn:
            return QLatin1String("File Path");
        default:
            return QVariant();
        }
    }
    return QVariant();
}

// --- ProjectHeaderPathModel --------------------------------------------------------------------

class ProjectHeaderPathsModel : public QAbstractListModel
{
public:
    ProjectHeaderPathsModel() = default;

    void configure(const ProjectExplorer::HeaderPaths &paths);
    void clear();

    enum Columns { TypeColumn, PathColumn, ColumnCount };

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    ProjectExplorer::HeaderPaths m_paths;
};

void ProjectHeaderPathsModel::configure(const ProjectExplorer::HeaderPaths &paths)
{
    emit layoutAboutToBeChanged();
    m_paths = paths;
    emit layoutChanged();
}

void ProjectHeaderPathsModel::clear()
{
    emit layoutAboutToBeChanged();
    m_paths.clear();
    emit layoutChanged();
}

int ProjectHeaderPathsModel::rowCount(const QModelIndex &/*parent*/) const
{
    return m_paths.size();
}

int ProjectHeaderPathsModel::columnCount(const QModelIndex &/*parent*/) const
{
    return ProjectFilesModel::ColumnCount;
}

QVariant ProjectHeaderPathsModel::data(const QModelIndex &index, int role) const
{
    if (role == Qt::DisplayRole) {
        const int row = index.row();
        const int column = index.column();
        if (column == TypeColumn)
            return CMI::Utils::toString(m_paths.at(row).type);
        if (column == PathColumn)
            return m_paths.at(row).path.toUserOutput();
    }
    return QVariant();
}

QVariant ProjectHeaderPathsModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
        switch (section) {
        case TypeColumn:
            return QLatin1String("Type");
        case PathColumn:
            return QLatin1String("Path");
        default:
            return QVariant();
        }
    }
    return QVariant();
}

// --- KeyValueModel ------------------------------------------------------------------------------

class KeyValueModel : public QAbstractListModel
{
public:
    using Table = QList<QPair<QString, QString>>;

    KeyValueModel() = default;

    void configure(const Table &table);
    void clear();

    enum Columns { KeyColumn, ValueColumn, ColumnCount };

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    Table m_table;
};

void KeyValueModel::configure(const Table &table)
{
    emit layoutAboutToBeChanged();
    m_table = table;
    emit layoutChanged();
}

void KeyValueModel::clear()
{
    emit layoutAboutToBeChanged();
    m_table.clear();
    emit layoutChanged();
}

int KeyValueModel::rowCount(const QModelIndex &/*parent*/) const
{
    return m_table.size();
}

int KeyValueModel::columnCount(const QModelIndex &/*parent*/) const
{
    return KeyValueModel::ColumnCount;
}

QVariant KeyValueModel::data(const QModelIndex &index, int role) const
{
    if (role == Qt::DisplayRole) {
        const int row = index.row();
        const int column = index.column();
        if (column == KeyColumn) {
            return m_table.at(row).first;
        } else if (column == ValueColumn) {
            return m_table.at(row).second;
        }
    }
    return QVariant();
}

QVariant KeyValueModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
        switch (section) {
        case KeyColumn:
            return QLatin1String("Key");
        case ValueColumn:
            return QLatin1String("Value");
        default:
            return QVariant();
        }
    }
    return QVariant();
}

// --- SnapshotModel ------------------------------------------------------------------------------

class SnapshotModel : public QAbstractListModel
{
public:
    SnapshotModel() = default;

    void configure(const Snapshot &snapshot);
    void setGlobalSnapshot(const Snapshot &snapshot);

    QModelIndex indexForDocument(const FilePath &filePath);

    enum Columns { SymbolCountColumn, SharedColumn, FilePathColumn, ColumnCount };

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    QList<Document::Ptr> m_documents;
    Snapshot m_globalSnapshot;
};

void SnapshotModel::configure(const Snapshot &snapshot)
{
    emit layoutAboutToBeChanged();
    m_documents = CMI::Utils::snapshotToList(snapshot);
    emit layoutChanged();
}

void SnapshotModel::setGlobalSnapshot(const Snapshot &snapshot)
{
    m_globalSnapshot = snapshot;
}

QModelIndex SnapshotModel::indexForDocument(const FilePath &filePath)
{
    for (int i = 0, total = m_documents.size(); i < total; ++i) {
        const Document::Ptr document = m_documents.at(i);
        if (document->filePath() == filePath)
            return index(i, FilePathColumn);
    }
    return {};
}

int SnapshotModel::rowCount(const QModelIndex &/*parent*/) const
{
    return m_documents.size();
}

int SnapshotModel::columnCount(const QModelIndex &/*parent*/) const
{
    return SnapshotModel::ColumnCount;
}

QVariant SnapshotModel::data(const QModelIndex &index, int role) const
{
    if (role == Qt::DisplayRole) {
        const int column = index.column();
        Document::Ptr document = m_documents.at(index.row());
        if (column == SymbolCountColumn) {
            return document->control()->symbolCount();
        } else if (column == SharedColumn) {
            Document::Ptr globalDocument = m_globalSnapshot.document(document->filePath());
            const bool isShared
                = globalDocument && globalDocument->fingerprint() == document->fingerprint();
            return CMI::Utils::toString(isShared);
        } else if (column == FilePathColumn) {
            return document->filePath().toUserOutput();
        }
    }
    return QVariant();
}

QVariant SnapshotModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
        switch (section) {
        case SymbolCountColumn:
            return QLatin1String("Symbols");
        case SharedColumn:
            return QLatin1String("Shared");
        case FilePathColumn:
            return QLatin1String("File Path");
        default:
            return QVariant();
        }
    }
    return QVariant();
}

// --- IncludesModel ------------------------------------------------------------------------------

static bool includesSorter(const Document::Include &i1,
                           const Document::Include &i2)
{
    return i1.line() < i2.line();
}

class IncludesModel : public QAbstractListModel
{
public:
    IncludesModel() = default;

    void configure(const QList<Document::Include> &includes);
    void clear();

    enum Columns { ResolvedOrNotColumn, LineNumberColumn, FilePathsColumn, ColumnCount };

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    QList<Document::Include> m_includes;
};

void IncludesModel::configure(const QList<Document::Include> &includes)
{
    emit layoutAboutToBeChanged();
    m_includes = includes;
    std::stable_sort(m_includes.begin(), m_includes.end(), includesSorter);
    emit layoutChanged();
}

void IncludesModel::clear()
{
    emit layoutAboutToBeChanged();
    m_includes.clear();
    emit layoutChanged();
}

int IncludesModel::rowCount(const QModelIndex &/*parent*/) const
{
    return m_includes.size();
}

int IncludesModel::columnCount(const QModelIndex &/*parent*/) const
{
    return IncludesModel::ColumnCount;
}

QVariant IncludesModel::data(const QModelIndex &index, int role) const
{
    if (role != Qt::DisplayRole && role != Qt::ForegroundRole)
        return QVariant();

    static const QBrush greenBrush(QColor(0, 139, 69));
    static const QBrush redBrush(QColor(205, 38, 38));

    const Document::Include include = m_includes.at(index.row());
    const FilePath resolvedFileName = include.resolvedFileName();
    const bool isResolved = !resolvedFileName.isEmpty();

    if (role == Qt::DisplayRole) {
        const int column = index.column();
        if (column == ResolvedOrNotColumn) {
            return CMI::Utils::toString(isResolved);
        } else if (column == LineNumberColumn) {
            return include.line();
        } else if (column == FilePathsColumn) {
            return QVariant(CMI::Utils::unresolvedFileNameWithDelimiters(include)
                            + QLatin1String(" --> ") + resolvedFileName.toUserOutput());
        }
    } else if (role == Qt::ForegroundRole) {
        return isResolved ? greenBrush : redBrush;
    }

    return QVariant();
}

QVariant IncludesModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
        switch (section) {
        case ResolvedOrNotColumn:
            return QLatin1String("Resolved");
        case LineNumberColumn:
            return QLatin1String("Line");
        case FilePathsColumn:
            return QLatin1String("File Paths");
        default:
            return QVariant();
        }
    }
    return QVariant();
}

// --- DiagnosticMessagesModel --------------------------------------------------------------------

static bool diagnosticMessagesModelSorter(const Document::DiagnosticMessage &m1,
                                          const Document::DiagnosticMessage &m2)
{
    return m1.line() < m2.line();
}

class DiagnosticMessagesModel : public QAbstractListModel
{
public:
    DiagnosticMessagesModel() = default;

    void configure(const QList<Document::DiagnosticMessage> &messages);
    void clear();

    enum Columns { LevelColumn, LineColumnNumberColumn, MessageColumn, ColumnCount };

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    QList<Document::DiagnosticMessage> m_messages;
};

void DiagnosticMessagesModel::configure(const QList<Document::DiagnosticMessage> &messages)
{
    emit layoutAboutToBeChanged();
    m_messages = messages;
    std::stable_sort(m_messages.begin(), m_messages.end(), diagnosticMessagesModelSorter);
    emit layoutChanged();
}

void DiagnosticMessagesModel::clear()
{
    emit layoutAboutToBeChanged();
    m_messages.clear();
    emit layoutChanged();
}

int DiagnosticMessagesModel::rowCount(const QModelIndex &/*parent*/) const
{
    return m_messages.size();
}

int DiagnosticMessagesModel::columnCount(const QModelIndex &/*parent*/) const
{
    return DiagnosticMessagesModel::ColumnCount;
}

QVariant DiagnosticMessagesModel::data(const QModelIndex &index, int role) const
{
    if (role != Qt::DisplayRole && role != Qt::ForegroundRole)
        return QVariant();

    static const QBrush yellowOrangeBrush(QColor(237, 145, 33));
    static const QBrush redBrush(QColor(205, 38, 38));
    static const QBrush darkRedBrushQColor(QColor(139, 0, 0));

    const Document::DiagnosticMessage message = m_messages.at(index.row());
    const auto level = static_cast<Document::DiagnosticMessage::Level>(message.level());

    if (role == Qt::DisplayRole) {
        const int column = index.column();
        if (column == LevelColumn) {
            return CMI::Utils::toString(level);
        } else if (column == LineColumnNumberColumn) {
            return QVariant(QString::number(message.line()) + QLatin1Char(':')
                            + QString::number(message.column()));
        } else if (column == MessageColumn) {
            return message.text();
        }
    } else if (role == Qt::ForegroundRole) {
        switch (level) {
        case Document::DiagnosticMessage::Warning:
            return yellowOrangeBrush;
        case Document::DiagnosticMessage::Error:
            return redBrush;
        case Document::DiagnosticMessage::Fatal:
            return darkRedBrushQColor;
        default:
            return QVariant();
        }
    }

    return QVariant();
}

QVariant DiagnosticMessagesModel::headerData(int section, Qt::Orientation orientation, int role)
    const
{
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
        switch (section) {
        case LevelColumn:
            return QLatin1String("Level");
        case LineColumnNumberColumn:
            return QLatin1String("Line:Column");
        case MessageColumn:
            return QLatin1String("Message");
        default:
            return QVariant();
        }
    }
    return QVariant();
}

// --- MacrosModel --------------------------------------------------------------------------------

class MacrosModel : public QAbstractListModel
{
public:
    MacrosModel() = default;

    void configure(const QList<CPlusPlus::Macro> &macros);
    void clear();

    enum Columns { LineNumberColumn, MacroColumn, ColumnCount };

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    QList<CPlusPlus::Macro> m_macros;
};

void MacrosModel::configure(const QList<CPlusPlus::Macro> &macros)
{
    emit layoutAboutToBeChanged();
    m_macros = macros;
    emit layoutChanged();
}

void MacrosModel::clear()
{
    emit layoutAboutToBeChanged();
    m_macros.clear();
    emit layoutChanged();
}

int MacrosModel::rowCount(const QModelIndex &/*parent*/) const
{
    return m_macros.size();
}

int MacrosModel::columnCount(const QModelIndex &/*parent*/) const
{
    return MacrosModel::ColumnCount;
}

QVariant MacrosModel::data(const QModelIndex &index, int role) const
{
    const int column = index.column();
    if (role == Qt::DisplayRole || (role == Qt::ToolTipRole && column == MacroColumn)) {
        const CPlusPlus::Macro macro = m_macros.at(index.row());
        if (column == LineNumberColumn)
            return macro.line();
        else if (column == MacroColumn)
            return macro.toString();
    } else if (role == Qt::TextAlignmentRole) {
        return QVariant::fromValue(Qt::AlignTop | Qt::AlignLeft);
    }
    return QVariant();
}

QVariant MacrosModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
        switch (section) {
        case LineNumberColumn:
            return QLatin1String("Line");
        case MacroColumn:
            return QLatin1String("Macro");
        default:
            return QVariant();
        }
    }
    return QVariant();
}

// --- SymbolsModel -------------------------------------------------------------------------------

class SymbolsModel : public QAbstractItemModel
{
public:
    SymbolsModel() = default;

    void configure(const Document::Ptr &document);
    void clear();

    enum Columns { SymbolColumn, LineNumberColumn, ColumnCount };

    QModelIndex index(int row, int column, const QModelIndex &parent) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    Document::Ptr m_document;
};

void SymbolsModel::configure(const Document::Ptr &document)
{
    QTC_CHECK(document);
    emit layoutAboutToBeChanged();
    m_document = document;
    emit layoutChanged();
}

void SymbolsModel::clear()
{
    emit layoutAboutToBeChanged();
    m_document.clear();
    emit layoutChanged();
}

static Symbol *indexToSymbol(const QModelIndex &index)
{
    return static_cast<Symbol*>(index.internalPointer());
}

static Scope *indexToScope(const QModelIndex &index)
{
    if (Symbol *symbol = indexToSymbol(index))
        return symbol->asScope();
    return nullptr;
}

QModelIndex SymbolsModel::index(int row, int column, const QModelIndex &parent) const
{
    Scope *scope = nullptr;
    if (parent.isValid())
        scope = indexToScope(parent);
    else if (m_document)
        scope = m_document->globalNamespace();

    if (scope) {
        if (row < scope->memberCount())
            return createIndex(row, column, scope->memberAt(row));
    }

    return {};
}

QModelIndex SymbolsModel::parent(const QModelIndex &child) const
{
    if (!child.isValid())
        return {};

    if (Symbol *symbol = indexToSymbol(child)) {
        if (Scope *scope = symbol->enclosingScope()) {
            const int row = DepthFinder()(m_document, scope);
            return createIndex(row, 0, scope);
        }
    }

    return {};
}

int SymbolsModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        if (Scope *scope = indexToScope(parent))
            return scope->memberCount();
    } else {
        if (m_document)
            return m_document->globalNamespace()->memberCount();
    }
    return 0;
}

int SymbolsModel::columnCount(const QModelIndex &) const
{
    return ColumnCount;
}

QVariant SymbolsModel::data(const QModelIndex &index, int role) const
{
    const int column = index.column();
    if (role == Qt::DisplayRole) {
        Symbol *symbol = indexToSymbol(index);
        if (!symbol)
            return QVariant();
        if (column == LineNumberColumn) {
            return symbol->line();
        } else if (column == SymbolColumn) {
            QString name = Overview().prettyName(symbol->name());
            if (name.isEmpty())
                name = QLatin1String(symbol->asBlock() ? "<block>" : "<no name>");
            return name;
        }
    }
    return QVariant();
}

QVariant SymbolsModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
        switch (section) {
        case SymbolColumn:
            return QLatin1String("Symbol");
        case LineNumberColumn:
            return QLatin1String("Line");
        default:
            return QVariant();
        }
    }
    return QVariant();
}

// --- TokensModel --------------------------------------------------------------------------------

class TokensModel : public QAbstractListModel
{
public:
    TokensModel() = default;

    void configure(TranslationUnit *translationUnit);
    void clear();

    enum Columns { SpelledColumn, KindColumn, IndexColumn, OffsetColumn, LineColumnNumberColumn,
                   BytesAndCodePointsColumn, GeneratedColumn, ExpandedColumn, WhiteSpaceColumn,
                   NewlineColumn, ColumnCount };

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    struct TokenInfo {
        Token token;
        int line;
        int column;
    };
    QList<TokenInfo> m_tokenInfos;
};

void TokensModel::configure(TranslationUnit *translationUnit)
{
    if (!translationUnit)
        return;

    emit layoutAboutToBeChanged();
    m_tokenInfos.clear();
    for (int i = 0, total = translationUnit->tokenCount(); i < total; ++i) {
        TokenInfo info;
        info.token = translationUnit->tokenAt(i);
        translationUnit->getPosition(info.token.utf16charsBegin(), &info.line, &info.column);
        m_tokenInfos.append(info);
    }
    emit layoutChanged();
}

void TokensModel::clear()
{
    emit layoutAboutToBeChanged();
    m_tokenInfos.clear();
    emit layoutChanged();
}

int TokensModel::rowCount(const QModelIndex &/*parent*/) const
{
    return m_tokenInfos.size();
}

int TokensModel::columnCount(const QModelIndex &/*parent*/) const
{
    return TokensModel::ColumnCount;
}

QVariant TokensModel::data(const QModelIndex &index, int role) const
{
    const int column = index.column();
    if (role == Qt::DisplayRole) {
        const TokenInfo info = m_tokenInfos.at(index.row());
        const Token token = info.token;
        if (column == SpelledColumn)
            return QString::fromUtf8(token.spell());
        else if (column == KindColumn)
            return CMI::Utils::toString(static_cast<Kind>(token.kind()));
        else if (column == IndexColumn)
            return index.row();
        else if (column == OffsetColumn)
            return token.bytesBegin();
        else if (column == LineColumnNumberColumn)
            return QString::fromLatin1("%1:%2").arg(CMI::Utils::toString(info.line),
                                                    CMI::Utils::toString(info.column));
        else if (column == BytesAndCodePointsColumn)
            return QString::fromLatin1("%1/%2").arg(CMI::Utils::toString(token.bytes()),
                                                    CMI::Utils::toString(token.utf16chars()));
        else if (column == GeneratedColumn)
            return CMI::Utils::toString(token.generated());
        else if (column == ExpandedColumn)
            return CMI::Utils::toString(token.expanded());
        else if (column == WhiteSpaceColumn)
            return CMI::Utils::toString(token.whitespace());
        else if (column == NewlineColumn)
            return CMI::Utils::toString(token.newline());
    } else if (role == Qt::TextAlignmentRole) {
        return QVariant::fromValue(Qt::AlignTop | Qt::AlignLeft);
    }
    return QVariant();
}

QVariant TokensModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
        switch (section) {
        case SpelledColumn:
            return QLatin1String("Spelled");
        case KindColumn:
            return QLatin1String("Kind");
        case IndexColumn:
            return QLatin1String("Index");
        case OffsetColumn:
            return QLatin1String("Offset");
        case LineColumnNumberColumn:
            return QLatin1String("Line:Column");
        case BytesAndCodePointsColumn:
            return QLatin1String("Bytes/Codepoints");
        case GeneratedColumn:
            return QLatin1String("Generated");
        case ExpandedColumn:
            return QLatin1String("Expanded");
        case WhiteSpaceColumn:
            return QLatin1String("Whitespace");
        case NewlineColumn:
            return QLatin1String("Newline");
        default:
            return QVariant();
        }
    }
    return QVariant();
}

// --- ProjectPartsModel --------------------------------------------------------------------------

class ProjectPartsModel : public QAbstractListModel
{
public:
    ProjectPartsModel() = default;

    void configure(const QList<ProjectInfo::ConstPtr> &projectInfos,
                   const ProjectPart::ConstPtr &currentEditorsProjectPart);

    QModelIndex indexForCurrentEditorsProjectPart() const;
    ProjectPart::ConstPtr projectPartForProjectId(const QString &projectPartId) const;

    enum Columns { PartNameColumn, PartFilePathColumn, ColumnCount };

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    QList<ProjectPart::ConstPtr> m_projectPartsList;
    int m_currentEditorsProjectPartIndex = -1;
};

void ProjectPartsModel::configure(const QList<ProjectInfo::ConstPtr> &projectInfos,
                                  const ProjectPart::ConstPtr &currentEditorsProjectPart)
{
    emit layoutAboutToBeChanged();
    m_projectPartsList.clear();
    for (const ProjectInfo::ConstPtr &info : std::as_const(projectInfos)) {
        const QList<ProjectPart::ConstPtr> projectParts = info->projectParts();
        for (const ProjectPart::ConstPtr &projectPart : projectParts) {
            if (!m_projectPartsList.contains(projectPart)) {
                m_projectPartsList << projectPart;
                if (projectPart == currentEditorsProjectPart)
                    m_currentEditorsProjectPartIndex = m_projectPartsList.size() - 1;
            }
        }
    }
    emit layoutChanged();
}

QModelIndex ProjectPartsModel::indexForCurrentEditorsProjectPart() const
{
    if (m_currentEditorsProjectPartIndex == -1)
        return {};
    return createIndex(m_currentEditorsProjectPartIndex, PartFilePathColumn);
}

ProjectPart::ConstPtr ProjectPartsModel::projectPartForProjectId(const QString &projectPartId) const
{
    for (const ProjectPart::ConstPtr &part : std::as_const(m_projectPartsList)) {
        if (part->id() == projectPartId)
            return part;
    }
    return ProjectPart::ConstPtr();
}

int ProjectPartsModel::rowCount(const QModelIndex &/*parent*/) const
{
    return m_projectPartsList.size();
}

int ProjectPartsModel::columnCount(const QModelIndex &/*parent*/) const
{
    return ProjectPartsModel::ColumnCount;
}

QVariant ProjectPartsModel::data(const QModelIndex &index, int role) const
{
    const int row = index.row();
    if (role == Qt::DisplayRole) {
        const int column = index.column();
        if (column == PartNameColumn)
            return m_projectPartsList.at(row)->displayName;
        if (column == PartFilePathColumn)
            return m_projectPartsList.at(row)->projectFile.toUserOutput();
    } else if (role == Qt::ForegroundRole) {
        if (!m_projectPartsList.at(row)->selectedForBuilding) {
            return QApplication::palette().color(QPalette::ColorGroup::Disabled,
                                                 QPalette::ColorRole::Text);
        }
    } else if (role == Qt::UserRole) {
        return m_projectPartsList.at(row)->id();
    }
    return QVariant();
}

QVariant ProjectPartsModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
        switch (section) {
        case PartNameColumn:
            return QLatin1String("Name");
        case PartFilePathColumn:
            return QLatin1String("Project File Path");
        default:
            return QVariant();
        }
    }
    return QVariant();
}

// --- WorkingCopyModel ---------------------------------------------------------------------------

class WorkingCopyModel : public QAbstractListModel
{
public:
    WorkingCopyModel() = default;

    void configure(const WorkingCopy &workingCopy);
    QModelIndex indexForFile(const FilePath &filePath);

    enum Columns { RevisionColumn, FilePathColumn, ColumnCount };

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    struct WorkingCopyEntry
    {
        WorkingCopyEntry(const FilePath &filePath, const QByteArray &source, unsigned revision)
            : filePath(filePath), source(source), revision(revision)
        {}

        FilePath filePath;
        QByteArray source;
        unsigned revision;
    };

    QList<WorkingCopyEntry> m_workingCopyList;
};

void WorkingCopyModel::configure(const WorkingCopy &workingCopy)
{
    emit layoutAboutToBeChanged();
    m_workingCopyList.clear();
    const WorkingCopy::Table &elements = workingCopy.elements();
    for (auto it = elements.cbegin(), end = elements.cend(); it != end; ++it)
        m_workingCopyList << WorkingCopyEntry(it.key(), it.value().first, it.value().second);

    emit layoutChanged();
}

QModelIndex WorkingCopyModel::indexForFile(const FilePath &filePath)
{
    for (int i = 0, total = m_workingCopyList.size(); i < total; ++i) {
        const WorkingCopyEntry entry = m_workingCopyList.at(i);
        if (entry.filePath == filePath)
            return index(i, FilePathColumn);
    }
    return {};
}

int WorkingCopyModel::rowCount(const QModelIndex &/*parent*/) const
{
    return m_workingCopyList.size();
}

int WorkingCopyModel::columnCount(const QModelIndex &/*parent*/) const
{
    return WorkingCopyModel::ColumnCount;
}

QVariant WorkingCopyModel::data(const QModelIndex &index, int role) const
{
    const int row = index.row();
    if (role == Qt::DisplayRole) {
        const int column = index.column();
        if (column == RevisionColumn)
            return m_workingCopyList.at(row).revision;
        else if (column == FilePathColumn)
            return m_workingCopyList.at(row).filePath.toUrlishString();
    } else if (role == Qt::UserRole) {
        return m_workingCopyList.at(row).source;
    }
    return QVariant();
}

QVariant WorkingCopyModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
        switch (section) {
        case RevisionColumn:
            return QLatin1String("Revision");
        case FilePathColumn:
            return QLatin1String("File Path");
        default:
            return QVariant();
        }
    }
    return QVariant();
}

// --- SnapshotInfo -------------------------------------------------------------------------------

class SnapshotInfo
{
public:
    enum Type { GlobalSnapshot, EditorSnapshot };
    SnapshotInfo(const Snapshot &snapshot, Type type)
        : snapshot(snapshot), type(type) {}

    Snapshot snapshot;
    Type type;
};

// --- CppCodeModelInspectorDialog ----------------------------------------------------------------

//
// This dialog is for DEBUGGING PURPOSES and thus NOT TRANSLATED.
//

class CppCodeModelInspectorDialog : public QDialog
{
public:
    CppCodeModelInspectorDialog();

private:
    void onSnapshotFilterChanged(const QString &pattern);
    void onSnapshotSelected(int row);
    void onDocumentSelected(const QModelIndex &current, const QModelIndex &);
    void onSymbolsViewExpandedOrCollapsed(const QModelIndex &);

    void onProjectPartFilterChanged(const QString &pattern);
    void onProjectPartSelected(const QModelIndex &current, const QModelIndex &);

    void onWorkingCopyFilterChanged(const QString &pattern);
    void onWorkingCopyDocumentSelected(const QModelIndex &current, const QModelIndex &);

    void refresh();

    void clearDocumentData();
    void updateDocumentData(const CPlusPlus::Document::Ptr &document);

    void clearProjectPartData();
    void updateProjectPartData(const ProjectPart::ConstPtr &part);

    bool event(QEvent *e) override;

private:
    QTabWidget *m_projectPartTab;
    QPlainTextEdit *m_partGeneralCompilerFlagsEdit;
    QPlainTextEdit *m_partToolchainDefinesEdit;
    QPlainTextEdit *m_partProjectDefinesEdit;
    QPlainTextEdit *m_partPrecompiledHeadersEdit;
    QComboBox *m_snapshotSelector;
    QTabWidget *m_docTab;
    QTreeView *m_docGeneralView;
    QTreeView *m_docIncludesView;
    QTreeView *m_docDiagnosticMessagesView;
    QTreeView *m_docDefinedMacrosView;
    QPlainTextEdit *m_docPreprocessedSourceEdit;
    QTreeView *m_docSymbolsView;
    QPlainTextEdit *m_workingCopySourceEdit;
    QCheckBox *m_selectEditorRelevantEntriesAfterRefreshCheckBox;
    QTreeView *m_partGeneralView;
    QTreeView *m_docTokensView;

    // Snapshots and Documents
    QList<SnapshotInfo> m_snapshotInfos;
    FilterableView *m_snapshotView;
    SnapshotModel m_snapshotModel;
    QSortFilterProxyModel m_proxySnapshotModel;
    KeyValueModel m_docGenericInfoModel;
    IncludesModel m_docIncludesModel;
    DiagnosticMessagesModel m_docDiagnosticMessagesModel;
    MacrosModel m_docMacrosModel;
    SymbolsModel m_docSymbolsModel;
    TokensModel m_docTokensModel;

    // Project Parts
    FilterableView *m_projectPartsView;
    ProjectPartsModel m_projectPartsModel;
    QSortFilterProxyModel m_proxyProjectPartsModel;
    KeyValueModel m_partGenericInfoModel;
    ProjectFilesModel m_projectFilesModel;
    ProjectHeaderPathsModel m_projectHeaderPathsModel;

    // Working Copy
    FilterableView *m_workingCopyView;
    WorkingCopyModel m_workingCopyModel;
    QSortFilterProxyModel m_proxyWorkingCopyModel;
};

CppCodeModelInspectorDialog::CppCodeModelInspectorDialog()
    : QDialog(ICore::dialogParent())
    , m_snapshotView(new FilterableView(this))
    , m_projectPartsView(new FilterableView(this))
    , m_workingCopyView(new FilterableView(this))
{
    resize(818, 756);
    setWindowTitle(QString::fromUtf8("C++ Code Model Inspector"));

    m_partGeneralView = new QTreeView;
    m_partGeneralView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    m_partGeneralCompilerFlagsEdit = new QPlainTextEdit;

    auto projectFilesView = new QTreeView;

    m_partToolchainDefinesEdit = new QPlainTextEdit;
    m_partToolchainDefinesEdit->setReadOnly(true);

    m_partProjectDefinesEdit = new QPlainTextEdit;
    m_partProjectDefinesEdit->setReadOnly(true);

    auto projectHeaderPathsView = new QTreeView;

    m_partPrecompiledHeadersEdit = new QPlainTextEdit;
    m_partPrecompiledHeadersEdit->setReadOnly(true);

    m_snapshotSelector = new QComboBox;
    QSizePolicy sizePolicy1(QSizePolicy::Preferred, QSizePolicy::Fixed);
    sizePolicy1.setHorizontalStretch(100);
    sizePolicy1.setVerticalStretch(0);
    m_snapshotSelector->setSizePolicy(sizePolicy1);

    m_docGeneralView = new QTreeView;
    m_docGeneralView->setAlternatingRowColors(true);
    m_docGeneralView->setTextElideMode(Qt::ElideMiddle);

    m_docIncludesView = new QTreeView;
    m_docIncludesView->setAlternatingRowColors(true);
    m_docIncludesView->setTextElideMode(Qt::ElideMiddle);

    m_docDiagnosticMessagesView = new QTreeView;
    m_docDiagnosticMessagesView->setAlternatingRowColors(true);
    m_docDiagnosticMessagesView->setTextElideMode(Qt::ElideMiddle);

    m_docDefinedMacrosView = new QTreeView;
    m_docDefinedMacrosView->setAlternatingRowColors(true);
    m_docDefinedMacrosView->setTextElideMode(Qt::ElideMiddle);

    m_docPreprocessedSourceEdit = new QPlainTextEdit;
    m_docPreprocessedSourceEdit->setReadOnly(true);

    m_docSymbolsView = new QTreeView;
    m_docSymbolsView->setAlternatingRowColors(true);
    m_docSymbolsView->setTextElideMode(Qt::ElideMiddle);

    m_docTokensView = new QTreeView;
    m_docTokensView->setAlternatingRowColors(true);
    m_docTokensView->setTextElideMode(Qt::ElideMiddle);

    m_workingCopySourceEdit = new QPlainTextEdit;
    m_workingCopySourceEdit->setReadOnly(true);
    m_workingCopySourceEdit->setTextInteractionFlags(Qt::TextSelectableByKeyboard|Qt::TextSelectableByMouse);

    m_selectEditorRelevantEntriesAfterRefreshCheckBox = new QCheckBox;
    m_selectEditorRelevantEntriesAfterRefreshCheckBox->setText("Select &editor relevant entries after refresh");
    m_selectEditorRelevantEntriesAfterRefreshCheckBox->setChecked(true);

    auto refreshButton = new QPushButton("&Refresh");
    auto closeButton = new QPushButton("Close");

    setAttribute(Qt::WA_DeleteOnClose);
    connect(Core::ICore::instance(), &Core::ICore::coreAboutToClose, this, &QWidget::close);

    m_partGeneralView->setSizePolicy(sizePolicyWithStretchFactor(2));
    m_partGeneralCompilerFlagsEdit->setSizePolicy(sizePolicyWithStretchFactor(1));

    m_proxySnapshotModel.setSourceModel(&m_snapshotModel);
    m_proxySnapshotModel.setFilterKeyColumn(SnapshotModel::FilePathColumn);
    m_snapshotView->setModel(&m_proxySnapshotModel);
    m_docGeneralView->setModel(&m_docGenericInfoModel);
    m_docIncludesView->setModel(&m_docIncludesModel);
    m_docDiagnosticMessagesView->setModel(&m_docDiagnosticMessagesModel);
    m_docDefinedMacrosView->setModel(&m_docMacrosModel);
    m_docSymbolsView->setModel(&m_docSymbolsModel);
    m_docTokensView->setModel(&m_docTokensModel);

    m_proxyProjectPartsModel.setSourceModel(&m_projectPartsModel);
    m_proxyProjectPartsModel.setFilterKeyColumn(ProjectPartsModel::PartFilePathColumn);
    m_projectPartsView->setModel(&m_proxyProjectPartsModel);
    m_partGeneralView->setModel(&m_partGenericInfoModel);
    projectFilesView->setModel(&m_projectFilesModel);
    projectHeaderPathsView->setModel(&m_projectHeaderPathsModel);

    m_proxyWorkingCopyModel.setSourceModel(&m_workingCopyModel);
    m_proxyWorkingCopyModel.setFilterKeyColumn(WorkingCopyModel::FilePathColumn);
    m_workingCopyView->setModel(&m_proxyWorkingCopyModel);

    using namespace Layouting;

    TabWidget projectPart {
        bindTo(&m_projectPartTab),
        Tab("&General",
            Row {
                m_partGeneralView,
                Column {
                    Tr::tr("Compiler Flags"),
                    m_partGeneralCompilerFlagsEdit,
                },
            }
        ),
        Tab("Project &Files", Column { projectFilesView }),
        Tab("&Defines",
            Column {
                Group {
                    title(QString("Toolchain Defines")),
                    Column { m_partToolchainDefinesEdit },
                },
                Group {
                    title(QString("Project Defines")),
                    Column { m_partProjectDefinesEdit },
                }
            }
        ),
        Tab("&Header Paths", Column { projectHeaderPathsView }),
        Tab("Pre&compiled Headers", Column { m_partPrecompiledHeadersEdit }),
    };

    TabWidget docTab {
        bindTo(&m_docTab),
        Tab("&General", Column { m_docGeneralView }),
        Tab("&Includes", Column { m_docIncludesView }),
        Tab("&Diagnostic Messages", Column { m_docDiagnosticMessagesView }),
        Tab("(Un)Defined &Macros", Column { m_docDefinedMacrosView }),
        Tab("P&reprocessed Source", Column { m_docPreprocessedSourceEdit }),
        Tab("&Symbols", Column { m_docSymbolsView }),
        Tab("&Tokens", Column { m_docTokensView }),
    };

    Column {
        TabWidget {
            Tab ("&Project Parts",
                Column {
                    Splitter {
                        m_projectPartsView,
                        projectPart,
                    },
                }
            ),
            Tab ("&Snapshots and Documents",
                Column {
                    Splitter {
                        Column {
                            Form { QString("Sn&apshot:"), m_snapshotSelector },
                            m_snapshotView,
                            noMargin,
                        }.emerge(),
                        docTab,
                    },
                }
            ),
            Tab ("&Working Copy",
                Column {
                    Splitter {
                        m_workingCopyView,
                        m_workingCopySourceEdit,
                    },
                }
            ),
        },
        Row {
            refreshButton,
            m_selectEditorRelevantEntriesAfterRefreshCheckBox,
            st,
            closeButton,
        }
    }.attachTo(this);

    QTC_CHECK(m_projectPartTab);
    m_projectPartTab->setCurrentIndex(3);

    connect(m_snapshotView->selectionModel(),
            &QItemSelectionModel::currentRowChanged,
            this, &CppCodeModelInspectorDialog::onDocumentSelected);
    connect(m_snapshotView, &FilterableView::filterChanged,
            this, &CppCodeModelInspectorDialog::onSnapshotFilterChanged);
    connect(m_snapshotSelector, &QComboBox::currentIndexChanged,
            this, &CppCodeModelInspectorDialog::onSnapshotSelected);
    connect(m_docSymbolsView, &QTreeView::expanded,
            this, &CppCodeModelInspectorDialog::onSymbolsViewExpandedOrCollapsed);
    connect(m_docSymbolsView, &QTreeView::collapsed,
            this, &CppCodeModelInspectorDialog::onSymbolsViewExpandedOrCollapsed);

    connect(m_projectPartsView->selectionModel(),
            &QItemSelectionModel::currentRowChanged,
            this, &CppCodeModelInspectorDialog::onProjectPartSelected);
    connect(m_projectPartsView, &FilterableView::filterChanged,
            this, &CppCodeModelInspectorDialog::onProjectPartFilterChanged);

    connect(m_workingCopyView->selectionModel(),
            &QItemSelectionModel::currentRowChanged,
            this, &CppCodeModelInspectorDialog::onWorkingCopyDocumentSelected);
    connect(m_workingCopyView, &FilterableView::filterChanged,
            this, &CppCodeModelInspectorDialog::onWorkingCopyFilterChanged);

    connect(refreshButton, &QAbstractButton::clicked, this, &CppCodeModelInspectorDialog::refresh);
    connect(closeButton, &QAbstractButton::clicked, this, &QWidget::close);

    refresh();
}

void CppCodeModelInspectorDialog::onSnapshotFilterChanged(const QString &pattern)
{
    m_proxySnapshotModel.setFilterWildcard(pattern);
}

void CppCodeModelInspectorDialog::onSnapshotSelected(int row)
{
    if (row < 0 || row >= m_snapshotInfos.size())
        return;

    m_snapshotView->clearFilter();
    const SnapshotInfo info = m_snapshotInfos.at(row);
    m_snapshotModel.configure(info.snapshot);
    m_snapshotView->resizeColumns(SnapshotModel::ColumnCount);

    if (info.type == SnapshotInfo::GlobalSnapshot) {
        // Select first document
        const QModelIndex index = m_proxySnapshotModel.index(0, SnapshotModel::FilePathColumn);
        m_snapshotView->selectIndex(index);
    } else if (info.type == SnapshotInfo::EditorSnapshot) {
        // Select first document, unless we can find the editor document
        QModelIndex index = m_snapshotModel.indexForDocument(fileInCurrentEditor());
        index = m_proxySnapshotModel.mapFromSource(index);
        if (!index.isValid())
            index = m_proxySnapshotModel.index(0, SnapshotModel::FilePathColumn);
        m_snapshotView->selectIndex(index);
    }
}

void CppCodeModelInspectorDialog::onDocumentSelected(const QModelIndex &current,
                                                     const QModelIndex &)
{
    if (current.isValid()) {
        const QModelIndex index = m_proxySnapshotModel.index(current.row(),
                                                             SnapshotModel::FilePathColumn);
        const FilePath filePath = FilePath::fromUserInput(
            m_proxySnapshotModel.data(index, Qt::DisplayRole).toString());
        const SnapshotInfo info = m_snapshotInfos.at(m_snapshotSelector->currentIndex());
        updateDocumentData(info.snapshot.document(filePath));
    } else {
        clearDocumentData();
    }
}

void CppCodeModelInspectorDialog::onSymbolsViewExpandedOrCollapsed(const QModelIndex &)
{
    resizeColumns<SymbolsModel>(m_docSymbolsView);
}

void CppCodeModelInspectorDialog::onProjectPartFilterChanged(const QString &pattern)
{
    m_proxyProjectPartsModel.setFilterWildcard(pattern);
}

void CppCodeModelInspectorDialog::onProjectPartSelected(const QModelIndex &current,
                                                        const QModelIndex &)
{
    if (current.isValid()) {
        QModelIndex index = m_proxyProjectPartsModel.mapToSource(current);
        if (index.isValid()) {
            index = m_projectPartsModel.index(index.row(), ProjectPartsModel::PartFilePathColumn);
            const QString projectPartId = m_projectPartsModel.data(index, Qt::UserRole).toString();
            updateProjectPartData(m_projectPartsModel.projectPartForProjectId(projectPartId));
        }
    } else {
        clearProjectPartData();
    }
}

void CppCodeModelInspectorDialog::onWorkingCopyFilterChanged(const QString &pattern)
{
    m_proxyWorkingCopyModel.setFilterWildcard(pattern);
}

void CppCodeModelInspectorDialog::onWorkingCopyDocumentSelected(const QModelIndex &current,
                                                                const QModelIndex &)
{
    if (current.isValid()) {
        const QModelIndex index = m_proxyWorkingCopyModel.mapToSource(current);
        if (index.isValid()) {
            const QString source
                = QString::fromUtf8(m_workingCopyModel.data(index, Qt::UserRole).toByteArray());
            m_workingCopySourceEdit->setPlainText(source);
        }
    } else {
        m_workingCopySourceEdit->clear();
    }
}

void CppCodeModelInspectorDialog::refresh()
{
    const int oldSnapshotIndex = m_snapshotSelector->currentIndex();
    const bool selectEditorRelevant
        = m_selectEditorRelevantEntriesAfterRefreshCheckBox->isChecked();

    // Snapshots and Documents
    m_snapshotInfos.clear();
    m_snapshotSelector->clear();

    const Snapshot globalSnapshot = CppModelManager::snapshot();
    CppCodeModelInspector::Dumper dumper(globalSnapshot);
    m_snapshotModel.setGlobalSnapshot(globalSnapshot);

    m_snapshotInfos.append(SnapshotInfo(globalSnapshot, SnapshotInfo::GlobalSnapshot));
    const QString globalSnapshotTitle
        = QString::fromLatin1("Global/Indexing Snapshot (%1 Documents)").arg(globalSnapshot.size());
    m_snapshotSelector->addItem(globalSnapshotTitle);
    dumper.dumpSnapshot(globalSnapshot, globalSnapshotTitle, /*isGlobalSnapshot=*/ true);

    CppEditorDocumentHandle *cppEditorDocument = nullptr;
    if (auto editor = TextEditor::BaseTextEditor::currentTextEditor()) {
        const FilePath editorFilePath = editor->document()->filePath();
        cppEditorDocument = CppModelManager::cppEditorDocument(editorFilePath);
        if (auto documentProcessor = CppModelManager::cppEditorDocumentProcessor(editorFilePath)) {
            const Snapshot editorSnapshot = documentProcessor->snapshot();
            m_snapshotInfos.append(SnapshotInfo(editorSnapshot, SnapshotInfo::EditorSnapshot));
            const QString editorSnapshotTitle
                = QString::fromLatin1("Current Editor's Snapshot (%1 Documents)")
                    .arg(editorSnapshot.size());
            dumper.dumpSnapshot(editorSnapshot, editorSnapshotTitle);
            m_snapshotSelector->addItem(editorSnapshotTitle);
        }
        auto cppEditorWidget = qobject_cast<CppEditorWidget *>(editor->editorWidget());
        if (cppEditorWidget) {
            SemanticInfo semanticInfo = cppEditorWidget->semanticInfo();
            Snapshot snapshot;

            // Add semantic info snapshot
            snapshot = semanticInfo.snapshot;
            m_snapshotInfos.append(SnapshotInfo(snapshot, SnapshotInfo::EditorSnapshot));
            m_snapshotSelector->addItem(
                QString::fromLatin1("Current Editor's Semantic Info Snapshot (%1 Documents)")
                    .arg(snapshot.size()));

            // Add a pseudo snapshot containing only the semantic info document since this document
            // is not part of the semantic snapshot.
            snapshot = Snapshot();
            snapshot.insert(cppEditorWidget->semanticInfo().doc);
            m_snapshotInfos.append(SnapshotInfo(snapshot, SnapshotInfo::EditorSnapshot));
            const QString snapshotTitle
                = QString::fromLatin1("Current Editor's Pseudo Snapshot with Semantic Info Document (%1 Documents)")
                    .arg(snapshot.size());
            dumper.dumpSnapshot(snapshot, snapshotTitle);
            m_snapshotSelector->addItem(snapshotTitle);
        }
    }

    int snapshotIndex = 0;
    if (selectEditorRelevant) {
        for (int i = 0, total = m_snapshotInfos.size(); i < total; ++i) {
            const SnapshotInfo info = m_snapshotInfos.at(i);
            if (info.type == SnapshotInfo::EditorSnapshot) {
                snapshotIndex = i;
                break;
            }
        }
    } else if (oldSnapshotIndex < m_snapshotInfos.size()) {
        snapshotIndex = oldSnapshotIndex;
    }
    m_snapshotSelector->setCurrentIndex(snapshotIndex);
    onSnapshotSelected(snapshotIndex);

    // Project Parts
    const ProjectPart::ConstPtr editorsProjectPart = cppEditorDocument
        ? cppEditorDocument->processor()->parser()->projectPartInfo().projectPart
        : ProjectPart::ConstPtr();

    const QList<ProjectInfo::ConstPtr> projectInfos = CppModelManager::projectInfos();
    dumper.dumpProjectInfos(projectInfos);
    m_projectPartsModel.configure(projectInfos, editorsProjectPart);
    m_projectPartsView->resizeColumns(ProjectPartsModel::ColumnCount);
    QModelIndex index = m_proxyProjectPartsModel.index(0, ProjectPartsModel::PartFilePathColumn);
    if (index.isValid()) {
        if (selectEditorRelevant && editorsProjectPart) {
            QModelIndex editorPartIndex = m_projectPartsModel.indexForCurrentEditorsProjectPart();
            editorPartIndex = m_proxyProjectPartsModel.mapFromSource(editorPartIndex);
            if (editorPartIndex.isValid())
                index = editorPartIndex;
        }
        m_projectPartsView->selectIndex(index);
    }

    // Working Copy
    const WorkingCopy workingCopy = CppModelManager::workingCopy();
    dumper.dumpWorkingCopy(workingCopy);
    m_workingCopyModel.configure(workingCopy);
    m_workingCopyView->resizeColumns(WorkingCopyModel::ColumnCount);
    if (workingCopy.size() > 0) {
        QModelIndex index = m_proxyWorkingCopyModel.index(0, WorkingCopyModel::FilePathColumn);
        if (selectEditorRelevant) {
            const QModelIndex eindex = m_workingCopyModel.indexForFile(fileInCurrentEditor());
            if (eindex.isValid())
                index = m_proxyWorkingCopyModel.mapFromSource(eindex);
        }
        m_workingCopyView->selectIndex(index);
    }

    // Merged entities
    dumper.dumpMergedEntities(CppModelManager::headerPaths(),
                              ProjectExplorer::Macro::toByteArray(CppModelManager::definedMacros()));
}

enum DocumentTabs {
    DocumentGeneralTab,
    DocumentIncludesTab,
    DocumentDiagnosticsTab,
    DocumentDefinedMacrosTab,
    DocumentPreprocessedSourceTab,
    DocumentSymbolsTab,
    DocumentTokensTab
};

static QString docTabName(int tabIndex, int numberOfEntries = -1)
{
    const char *names[] = {
        "&General",
        "&Includes",
        "&Diagnostic Messages",
        "(Un)Defined &Macros",
        "P&reprocessed Source",
        "&Symbols",
        "&Tokens"
    };
    QString result = QLatin1String(names[tabIndex]);
    if (numberOfEntries != -1)
        result += QString::fromLatin1(" (%1)").arg(numberOfEntries);
    return result;
}

void CppCodeModelInspectorDialog::clearDocumentData()
{
    m_docGenericInfoModel.clear();

    m_docTab->setTabText(DocumentIncludesTab, docTabName(DocumentIncludesTab));
    m_docIncludesModel.clear();

    m_docTab->setTabText(DocumentDiagnosticsTab, docTabName(DocumentDiagnosticsTab));
    m_docDiagnosticMessagesModel.clear();

    m_docTab->setTabText(DocumentDefinedMacrosTab, docTabName(DocumentDefinedMacrosTab));
    m_docMacrosModel.clear();

    m_docPreprocessedSourceEdit->clear();

    m_docSymbolsModel.clear();

    m_docTab->setTabText(DocumentTokensTab, docTabName(DocumentTokensTab));
    m_docTokensModel.clear();
}

void CppCodeModelInspectorDialog::updateDocumentData(const Document::Ptr &document)
{
    QTC_ASSERT(document, return);

    // General
    const KeyValueModel::Table table = {
        {QString::fromLatin1("File Path"), document->filePath().toUserOutput()},
        {QString::fromLatin1("Last Modified"), CMI::Utils::toString(document->lastModified())},
        {QString::fromLatin1("Revision"), CMI::Utils::toString(document->revision())},
        {QString::fromLatin1("Editor Revision"), CMI::Utils::toString(document->editorRevision())},
        {QString::fromLatin1("Check Mode"), CMI::Utils::toString(document->checkMode())},
        {QString::fromLatin1("Tokenized"), CMI::Utils::toString(document->isTokenized())},
        {QString::fromLatin1("Parsed"), CMI::Utils::toString(document->isParsed())},
        {QString::fromLatin1("Project Parts"), CMI::Utils::partsForFile(document->filePath())}
    };
    m_docGenericInfoModel.configure(table);
    resizeColumns<KeyValueModel>(m_docGeneralView);

    // Includes
    m_docIncludesModel.configure(document->resolvedIncludes() + document->unresolvedIncludes());
    resizeColumns<IncludesModel>(m_docIncludesView);
    m_docTab->setTabText(DocumentIncludesTab,
        docTabName(DocumentIncludesTab, m_docIncludesModel.rowCount()));

    // Diagnostic Messages
    m_docDiagnosticMessagesModel.configure(document->diagnosticMessages());
    resizeColumns<DiagnosticMessagesModel>(m_docDiagnosticMessagesView);
    m_docTab->setTabText(DocumentDiagnosticsTab,
        docTabName(DocumentDiagnosticsTab, m_docDiagnosticMessagesModel.rowCount()));

    // Macros
    m_docMacrosModel.configure(document->definedMacros());
    resizeColumns<MacrosModel>(m_docDefinedMacrosView);
    m_docTab->setTabText(DocumentDefinedMacrosTab,
        docTabName(DocumentDefinedMacrosTab, m_docMacrosModel.rowCount()));

    // Source
    m_docPreprocessedSourceEdit->setPlainText(QString::fromUtf8(document->utf8Source()));

    // Symbols
    m_docSymbolsModel.configure(document);
    resizeColumns<SymbolsModel>(m_docSymbolsView);

    // Tokens
    m_docTokensModel.configure(document->translationUnit());
    resizeColumns<TokensModel>(m_docTokensView);
    m_docTab->setTabText(DocumentTokensTab,
        docTabName(DocumentTokensTab, m_docTokensModel.rowCount()));
}

enum ProjectPartTabs {
    ProjectPartGeneralTab,
    ProjectPartFilesTab,
    ProjectPartDefinesTab,
    ProjectPartHeaderPathsTab,
    ProjectPartPrecompiledHeadersTab
};

static QString partTabName(int tabIndex, int numberOfEntries = -1)
{
    const char *names[] = {
        "&General",
        "Project &Files",
        "&Defines",
        "&Header Paths",
        "Pre&compiled Headers"
    };
    QString result = QLatin1String(names[tabIndex]);
    if (numberOfEntries != -1)
        result += QString::fromLatin1(" (%1)").arg(numberOfEntries);
    return result;
}

void CppCodeModelInspectorDialog::clearProjectPartData()
{
    m_partGenericInfoModel.clear();
    m_projectFilesModel.clear();
    m_projectHeaderPathsModel.clear();

    m_projectPartTab->setTabText(ProjectPartFilesTab, partTabName(ProjectPartFilesTab));

    m_partToolchainDefinesEdit->clear();
    m_partProjectDefinesEdit->clear();
    m_projectPartTab->setTabText(ProjectPartDefinesTab, partTabName(ProjectPartDefinesTab));

    m_projectPartTab->setTabText(ProjectPartHeaderPathsTab,
                                     partTabName(ProjectPartHeaderPathsTab));

    m_partPrecompiledHeadersEdit->clear();
    m_projectPartTab->setTabText(ProjectPartPrecompiledHeadersTab,
                                     partTabName(ProjectPartPrecompiledHeadersTab));
}

static int defineCount(const ProjectExplorer::Macros &macros)
{
    using ProjectExplorer::Macro;
    return int(std::count_if(
                   macros.begin(),
                   macros.end(),
                   [](const Macro &macro) { return macro.type == ProjectExplorer::MacroType::Define; }));
}

void CppCodeModelInspectorDialog::updateProjectPartData(const ProjectPart::ConstPtr &part)
{
    QTC_ASSERT(part, return);

    // General
    QString projectName = QLatin1String("<None>");
    QString projectFilePath = QLatin1String("<None>");
    if (part->hasProject()) {
        projectFilePath = part->topLevelProject.toUserOutput();
        if (const ProjectExplorer::Project * const project = part->project())
            projectName = project->displayName();
    }
    const QString callGroupId = part->callGroupId.isEmpty() ? QString::fromLatin1("<None>")
                                                            : part->callGroupId;
    const QString buildSystemTarget
            = part->buildSystemTarget.isEmpty() ? QString::fromLatin1("<None>")
                                                : part->buildSystemTarget;

    const QString precompiledHeaders = part->precompiledHeaders.isEmpty()
            ? QString::fromLatin1("<None>")
            : part->precompiledHeaders.toUserOutput(",");

    KeyValueModel::Table table = {
        {QString::fromLatin1("Project Part Name"), part->displayName},
        {QString::fromLatin1("Project Part File"), part->projectFileLocation()},
        {QString::fromLatin1("Project Name"), projectName},
        {QString::fromLatin1("Project File"), projectFilePath},
        {QString::fromLatin1("Callgroup Id"), callGroupId},
        {QString::fromLatin1("Precompiled Headers"), precompiledHeaders},
        {QString::fromLatin1("Selected For Building"), CMI::Utils::toString(part->selectedForBuilding)},
        {QString::fromLatin1("Buildsystem Target"), buildSystemTarget},
        {QString::fromLatin1("Build Target Type"), CMI::Utils::toString(part->buildTargetType)},
        {QString::fromLatin1("ToolChain Type"), part->toolchainType.toString()},
        {QString::fromLatin1("ToolChain Target Triple"), part->toolchainTargetTriple},
        {QString::fromLatin1("ToolChain Word Width"), CMI::Utils::toString(part->toolchainAbi.wordWidth())},
        {QString::fromLatin1("ToolChain Install Dir"), part->toolchainInstallDir.toUserOutput()},
        {QString::fromLatin1("Language Version"), CMI::Utils::toString(part->languageVersion)},
        {QString::fromLatin1("Language Extensions"), CMI::Utils::toString(part->languageExtensions)},
        {QString::fromLatin1("Qt Version"), CMI::Utils::toString(part->qtVersion)}
    };
    if (!part->projectConfigFile.isEmpty())
        table.prepend({QString::fromLatin1("Project Config File"), part->projectConfigFile.toUserOutput()});
    m_partGenericInfoModel.configure(table);
    resizeColumns<KeyValueModel>(m_partGeneralView);

    // Compiler Flags
    m_partGeneralCompilerFlagsEdit->setPlainText(part->compilerFlags.join("\n"));

    // Project Files
    m_projectFilesModel.configure(part->files);
    m_projectPartTab->setTabText(ProjectPartFilesTab,
        partTabName(ProjectPartFilesTab, part->files.size()));

    int numberOfDefines = defineCount(part->toolchainMacros) + defineCount(part->projectMacros);

    m_partToolchainDefinesEdit->setPlainText(QString::fromUtf8(ProjectExplorer::Macro::toByteArray(part->toolchainMacros)));
    m_partProjectDefinesEdit->setPlainText(QString::fromUtf8(ProjectExplorer::Macro::toByteArray(part->projectMacros)));
    m_projectPartTab->setTabText(ProjectPartDefinesTab,
        partTabName(ProjectPartDefinesTab, numberOfDefines));

    // Header Paths
    m_projectHeaderPathsModel.configure(part->headerPaths);
    m_projectPartTab->setTabText(ProjectPartHeaderPathsTab,
        partTabName(ProjectPartHeaderPathsTab, part->headerPaths.size()));

    // Precompiled Headers
    m_partPrecompiledHeadersEdit->setPlainText(part->precompiledHeaders.toUserOutput("\n"));
    m_projectPartTab->setTabText(ProjectPartPrecompiledHeadersTab,
        partTabName(ProjectPartPrecompiledHeadersTab, part->precompiledHeaders.size()));
}

bool CppCodeModelInspectorDialog::event(QEvent *e)
{
    if (e->type() == QEvent::ShortcutOverride) {
        auto ke = static_cast<QKeyEvent *>(e);
        if (ke->key() == Qt::Key_Escape && !ke->modifiers()) {
            ke->accept();
            close();
            return false;
        }
    }
    return QDialog::event(e);
}

void inspectCppCodeModel()
{
    static QPointer<CppCodeModelInspectorDialog> theCppCodeModelInspectorDialog;

    if (theCppCodeModelInspectorDialog) {
        ICore::raiseWindow(theCppCodeModelInspectorDialog);
    } else {
        theCppCodeModelInspectorDialog = new CppCodeModelInspectorDialog;
        ICore::registerWindow(theCppCodeModelInspectorDialog, Context("CppEditor.Inspector"));
        theCppCodeModelInspectorDialog->show();
    }
}

} // CppEditor::Internal

#include "cppcodemodelinspectordialog.moc"
