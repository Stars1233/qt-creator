// Copyright (C) 2025 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include "../utils_global.h"

#include <QAbstractScrollArea>
#include <QAbstractTextDocumentLayout>
#include <QTextBlockUserData>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>

QT_BEGIN_NAMESPACE
class QTextCharFormat;
class QMimeData;
QT_END_NAMESPACE

namespace Utils {

class PlainTextDocumentLayout;
class PlainTextEditPrivate;

class QTCREATOR_UTILS_EXPORT PlainTextEdit : public QAbstractScrollArea
{
    Q_OBJECT
    Q_PROPERTY(bool tabChangesFocus READ tabChangesFocus WRITE setTabChangesFocus)
    Q_PROPERTY(QString documentTitle READ documentTitle WRITE setDocumentTitle)
    Q_PROPERTY(bool undoRedoEnabled READ isUndoRedoEnabled WRITE setUndoRedoEnabled)
    Q_PROPERTY(LineWrapMode lineWrapMode READ lineWrapMode WRITE setLineWrapMode)
    QDOC_PROPERTY(QTextOption::WrapMode wordWrapMode READ wordWrapMode WRITE setWordWrapMode)
    Q_PROPERTY(bool readOnly READ isReadOnly WRITE setReadOnly)
    Q_PROPERTY(QString plainText READ toPlainText WRITE setPlainText NOTIFY textChanged USER true)
    Q_PROPERTY(bool overwriteMode READ overwriteMode WRITE setOverwriteMode)
    Q_PROPERTY(qreal tabStopDistance READ tabStopDistance WRITE setTabStopDistance)
    Q_PROPERTY(int cursorWidth READ cursorWidth WRITE setCursorWidth)
    Q_PROPERTY(Qt::TextInteractionFlags textInteractionFlags READ textInteractionFlags
                   WRITE setTextInteractionFlags)
    Q_PROPERTY(int blockCount READ blockCount)
    Q_PROPERTY(int maximumBlockCount READ maximumBlockCount WRITE setMaximumBlockCount)
    Q_PROPERTY(bool backgroundVisible READ backgroundVisible WRITE setBackgroundVisible)
    Q_PROPERTY(bool centerOnScroll READ centerOnScroll WRITE setCenterOnScroll)
    Q_PROPERTY(QString placeholderText READ placeholderText WRITE setPlaceholderText)

public:
    enum LineWrapMode {
        NoWrap,
        WidgetWidth
    };
    Q_ENUM(LineWrapMode)

    explicit PlainTextEdit(QWidget *parent = nullptr);
    explicit PlainTextEdit(const QString &text, QWidget *parent = nullptr);
    virtual ~PlainTextEdit();

    void setDocument(QTextDocument *document);
    QTextDocument *document() const;

    void setPlaceholderText(const QString &placeholderText);
    QString placeholderText() const;

    void setTextCursor(const QTextCursor &cursor);
    QTextCursor textCursor() const;
    Q_INVOKABLE QVariant variantTextCursor() const; // needed for testing with Squish

    bool isReadOnly() const;
    void setReadOnly(bool ro);

    void setTextInteractionFlags(Qt::TextInteractionFlags flags);
    Qt::TextInteractionFlags textInteractionFlags() const;

    void mergeCurrentCharFormat(const QTextCharFormat &modifier);
    void setCurrentCharFormat(const QTextCharFormat &format);
    QTextCharFormat currentCharFormat() const;

    bool tabChangesFocus() const;
    void setTabChangesFocus(bool b);

    inline void setDocumentTitle(const QString &title)
    { document()->setMetaInformation(QTextDocument::DocumentTitle, title); }
    inline QString documentTitle() const
    { return document()->metaInformation(QTextDocument::DocumentTitle); }

    inline bool isUndoRedoEnabled() const
    { return document()->isUndoRedoEnabled(); }
    inline void setUndoRedoEnabled(bool enable)
    { document()->setUndoRedoEnabled(enable); }

    inline void setMaximumBlockCount(int maximum)
    { document()->setMaximumBlockCount(maximum); }
    inline int maximumBlockCount() const
    { return document()->maximumBlockCount(); }


    LineWrapMode lineWrapMode() const;
    void setLineWrapMode(LineWrapMode mode);

    QTextOption::WrapMode wordWrapMode() const;
    void setWordWrapMode(QTextOption::WrapMode policy);

    void setBackgroundVisible(bool visible);
    bool backgroundVisible() const;

    void setCenterOnScroll(bool enabled);
    bool centerOnScroll() const;

    bool find(const QString &exp, QTextDocument::FindFlags options = QTextDocument::FindFlags());
#if QT_CONFIG(regularexpression)
    bool find(const QRegularExpression &exp, QTextDocument::FindFlags options = QTextDocument::FindFlags());
#endif

    inline QString toPlainText() const
    { return document()->toPlainText(); }

    void setTopBlock(const QTextBlock &block);
    void ensureCursorVisible();

    virtual QVariant loadResource(int type, const QUrl &name);
#ifndef QT_NO_CONTEXTMENU
    QMenu *createStandardContextMenu();
    QMenu *createStandardContextMenu(const QPoint &position);
#endif

    QTextCursor cursorForPosition(const QPoint &pos) const;
    QRect cursorRect(const QTextCursor &cursor) const;
    Q_INVOKABLE QRect cursorRect() const; // Q_INVOKABLE needed for testing with Squish

    QString anchorAt(const QPoint &pos) const;

    bool overwriteMode() const;
    void setOverwriteMode(bool overwrite);

    qreal tabStopDistance() const;
    void setTabStopDistance(qreal distance);

    int cursorWidth() const;
    void setCursorWidth(int width);

    void setExtraSelections(const QList<QTextEdit::ExtraSelection> &selections);
    QList<QTextEdit::ExtraSelection> extraSelections() const;

    void moveCursor(QTextCursor::MoveOperation operation, QTextCursor::MoveMode mode = QTextCursor::MoveAnchor);

    bool canPaste() const;

    void print(QPagedPaintDevice *printer) const;

    int blockCount() const;
    QVariant inputMethodQuery(Qt::InputMethodQuery property) const override;
    Q_INVOKABLE QVariant inputMethodQuery(Qt::InputMethodQuery query, QVariant argument) const;

    PlainTextDocumentLayout *editorLayout() const;

public Q_SLOTS:

    void setPlainText(const QString &text);

#ifndef QT_NO_CLIPBOARD
    void cut();
    void copy();
    void paste();
#endif

    void undo();
    void redo();

    void clear();
    void selectAll();

    void insertPlainText(const QString &text);

    void appendPlainText(const QString &text);
    void appendHtml(const QString &html);

    void centerCursor();

    void zoomIn(int range = 1);
    void zoomOut(int range = 1);

Q_SIGNALS:
    void textChanged();
    void undoAvailable(bool b);
    void redoAvailable(bool b);
    void copyAvailable(bool b);
    void selectionChanged();
    void cursorPositionChanged();

    void updateRequest(const QRect &rect, int dy);
    void blockCountChanged(int newBlockCount);
    void modificationChanged(bool);

protected:
    virtual bool event(QEvent *e) override;
    virtual void timerEvent(QTimerEvent *e) override;
    virtual void keyPressEvent(QKeyEvent *e) override;
    virtual void keyReleaseEvent(QKeyEvent *e) override;
    virtual void resizeEvent(QResizeEvent *e) override;
    virtual void paintEvent(QPaintEvent *e) override;
    virtual void mousePressEvent(QMouseEvent *e) override;
    virtual void mouseMoveEvent(QMouseEvent *e) override;
    virtual void mouseReleaseEvent(QMouseEvent *e) override;
    virtual void mouseDoubleClickEvent(QMouseEvent *e) override;
    virtual bool focusNextPrevChild(bool next) override;
#ifndef QT_NO_CONTEXTMENU
    virtual void contextMenuEvent(QContextMenuEvent *e) override;
#endif
#if QT_CONFIG(draganddrop)
    virtual void dragEnterEvent(QDragEnterEvent *e) override;
    virtual void dragLeaveEvent(QDragLeaveEvent *e) override;
    virtual void dragMoveEvent(QDragMoveEvent *e) override;
    virtual void dropEvent(QDropEvent *e) override;
#endif
    virtual void focusInEvent(QFocusEvent *e) override;
    virtual void focusOutEvent(QFocusEvent *e) override;
    virtual void showEvent(QShowEvent *) override;
    virtual void changeEvent(QEvent *e) override;
#if QT_CONFIG(wheelevent)
    virtual void wheelEvent(QWheelEvent *e) override;
#endif

    virtual QMimeData *createMimeDataFromSelection() const;
    virtual bool canInsertFromMimeData(const QMimeData *source) const;
    virtual void insertFromMimeData(const QMimeData *source);

    virtual void inputMethodEvent(QInputMethodEvent *) override;

    // PlainTextEdit(PlainTextEditPrivate &dd, QWidget *parent);

    virtual void scrollContentsBy(int dx, int dy) override;
    virtual void doSetTextCursor(const QTextCursor &cursor);

    QTextBlock firstVisibleBlock() const;
    QPointF contentOffset() const;
    QRectF blockBoundingRect(const QTextBlock &block) const;
    QRectF blockBoundingGeometry(const QTextBlock &block) const;
    QAbstractTextDocumentLayout::PaintContext getPaintContext() const;

    void zoomInF(float range);

private:
    Q_DISABLE_COPY(PlainTextEdit)

    friend class PlainTextEditControl;
    friend class PlainTextEditPrivate;
    std::unique_ptr<PlainTextEditPrivate> d;
};

class PlainTextDocumentLayoutPrivate;

class QTCREATOR_UTILS_EXPORT PlainTextDocumentLayout : public QAbstractTextDocumentLayout
{
    Q_OBJECT
    Q_PROPERTY(int cursorWidth READ cursorWidth WRITE setCursorWidth)

public:
    PlainTextDocumentLayout(QTextDocument *document);
    ~PlainTextDocumentLayout();

    void draw(QPainter *, const PaintContext &) override;
    int hitTest(const QPointF &, Qt::HitTestAccuracy ) const override;

    int pageCount() const override;
    QSizeF documentSize() const override;

    QRectF frameBoundingRect(QTextFrame *) const override;
    QRectF blockBoundingRect(const QTextBlock &block) const override;

    void ensureBlockLayout(const QTextBlock &block) const;

    void setCursorWidth(int width);
    int cursorWidth() const;

    void requestUpdate();

    virtual QTextLayout *blockLayout(const QTextBlock &block) const;
    virtual void clearBlockLayout(QTextBlock &block) const;
    virtual int blockLineCount(const QTextBlock &block) const;
    virtual void setBlockLineCount(QTextBlock &block, int lineCount) const;
    virtual void setBlockLayedOut(const QTextBlock &block) const;
    virtual int lineCount() const;
    virtual int firstLineNumberOf(const QTextBlock &block) const;
    virtual QTextBlock findBlockByLineNumber(int lineNumber) const;
    virtual int additionalBlockHeight(const QTextBlock &block) const;
    virtual QRectF replacementBlockBoundingRect(const QTextBlock &block) const;
    virtual int relativeLineSpacing() const;
    virtual int lineSpacing() const;
    virtual bool moveCursor(
        QTextCursor &cursor,
        QTextCursor::MoveOperation operation,
        QTextCursor::MoveMode mode = QTextCursor::MoveAnchor,
        int steps = 1) const;

signals:
    void documentContentsChanged(int from, int charsRemoved, int charsAdded);
    void blockVisibilityChanged(const QTextBlock &block);
    void additionalBlockHeightChanged(const QTextBlock &block, int additionalHeight);

protected:
    void documentChanged(int from, int charsRemoved, int charsAdded) override;
    virtual void clearBlockLayout(QTextBlock &start, QTextBlock &end, bool &blockVisibilityChanged) const;

protected:
    void setTextWidth(qreal newWidth);
    virtual void relayout();
    qreal textWidth() const;
    void layoutBlock(const QTextBlock &block);
    qreal blockWidth(const QTextBlock &block);

    friend class PlainTextEdit;
    friend class PlainTextEditPrivate;
    friend class PlainTextDocumentLayoutPrivate;
    friend class TextEditorLayout;

    std::unique_ptr<PlainTextDocumentLayoutPrivate> d;
};

} // namespace Utils
