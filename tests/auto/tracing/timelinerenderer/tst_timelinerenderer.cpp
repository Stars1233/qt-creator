// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include <QTest>
#include <tracing/timelinemodelaggregator.h>
#include <tracing/timelinerenderer_p.h>

using namespace Timeline;

class DummyRenderer : public TimelineRenderer {
    friend class tst_TimelineRenderer;
};

class DummyModel : public TimelineModel {
public:
    DummyModel(TimelineModelAggregator *parent) : TimelineModel(parent) {}

    void loadData()
    {
        setCollapsedRowCount(3);
        setExpandedRowCount(3);
        for (int i = 0; i < 10; ++i)
            insert(i, i, i);
        for (int i = 0; i < 10; ++i)
            insert(i, 10 - i, i);
        computeNesting();
        emit contentChanged();
    }
};

class tst_TimelineRenderer : public QObject
{
    Q_OBJECT

private:
    void testMouseEvents(DummyRenderer *renderer, int x, int y);
    TimelineModelAggregator aggregator;

private slots:
    void updatePaintNode();
    void mouseEvents();
};

void tst_TimelineRenderer::updatePaintNode()
{
    DummyRenderer renderer;
    QCOMPARE(renderer.updatePaintNode(0, 0), static_cast<QSGNode *>(0));
    DummyModel model(&aggregator);
    renderer.setModel(&model);
    QCOMPARE(renderer.updatePaintNode(0, 0), static_cast<QSGNode *>(0));
    model.loadData();
    QCOMPARE(renderer.updatePaintNode(0, 0), static_cast<QSGNode *>(0));
    TimelineZoomControl zoomer;
    renderer.setZoomer(&zoomer);
    QCOMPARE(renderer.updatePaintNode(0, 0), static_cast<QSGNode *>(0));
    zoomer.setTrace(0, 10);
    QSGNode *node = renderer.updatePaintNode(0, 0);
    QVERIFY(node != 0);
    QCOMPARE(renderer.updatePaintNode(node, 0), node);
    renderer.setModelDirty();
    QCOMPARE(renderer.updatePaintNode(node, 0), node);
    renderer.setRowHeightsDirty();
    QCOMPARE(renderer.updatePaintNode(node, 0), node);
    delete node;
}

void tst_TimelineRenderer::testMouseEvents(DummyRenderer *renderer, int x, int y)
{
    QMouseEvent event(QMouseEvent::MouseMove, QPointF(x - 1, y), QCursor::pos(), Qt::NoButton,
                      Qt::NoButton, Qt::NoModifier);
    renderer->mouseMoveEvent(&event);

    QHoverEvent hover(QMouseEvent::HoverMove, QPointF(x, y), QPointF(x, y), QPointF(x - 1, y));
    renderer->hoverMoveEvent(&hover);
}

void tst_TimelineRenderer::mouseEvents()
{
    DummyRenderer renderer;
    QCOMPARE(renderer.selectedItem(), -1);
    QCOMPARE(renderer.selectionLocked(), true);
    testMouseEvents(&renderer, 1, 1); // make sure it doesn't crash without model and zoomer
    QCOMPARE(renderer.selectedItem(), -1);
    QCOMPARE(renderer.selectionLocked(), true);
    renderer.setWidth(10);
    renderer.setHeight(90);
    testMouseEvents(&renderer, 1, 1);
    QCOMPARE(renderer.selectedItem(), -1);
    QCOMPARE(renderer.selectionLocked(), true);

    TimelineZoomControl zoomer;
    renderer.setZoomer(&zoomer);
    testMouseEvents(&renderer, 1, 1);
    QCOMPARE(renderer.selectedItem(), -1);
    QCOMPARE(renderer.selectionLocked(), true);

    DummyModel model(&aggregator);
    renderer.setModel(&model);
    testMouseEvents(&renderer, 1, 1);
    QCOMPARE(renderer.selectedItem(), -1);
    QCOMPARE(renderer.selectionLocked(), true);

    zoomer.setTrace(0, 10);
    testMouseEvents(&renderer, 1, 1);
    QCOMPARE(renderer.selectedItem(), -1);
    QCOMPARE(renderer.selectionLocked(), true);

    model.loadData();
    testMouseEvents(&renderer, 1, 1);
    QCOMPARE(renderer.selectedItem(), 2);
    QCOMPARE(renderer.selectionLocked(), true);

    model.setExpanded(true);
    testMouseEvents(&renderer, 1, 1);
    QCOMPARE(renderer.selectedItem(), 2);
    QCOMPARE(renderer.selectionLocked(), true); // Don't toggle locked status by clicking same item
    renderer.setSelectionLocked(false);
    testMouseEvents(&renderer, 1, 1);
    QCOMPARE(renderer.selectedItem(), 2);
    QCOMPARE(renderer.selectionLocked(), false);
    renderer.setSelectionLocked(true);
    testMouseEvents(&renderer, 1, 40);
    QCOMPARE(renderer.selectedItem(), -1);
    QCOMPARE(renderer.selectionLocked(), true); // Don't unset locked by clicking empty space
    renderer.setSelectionLocked(false);
    testMouseEvents(&renderer, 1, 400);
    QCOMPARE(renderer.selectedItem(), -1);
    QCOMPARE(renderer.selectionLocked(), false);
    testMouseEvents(&renderer, 10, 1);
    QCOMPARE(renderer.selectedItem(), 14);
    QCOMPARE(renderer.selectionLocked(), false);
    renderer.setSelectionLocked(true);

    renderer.selectNextFromSelectionId(4);
    QCOMPARE(renderer.selectedItem(), 8);
    QCOMPARE(renderer.selectionLocked(), true); // no toggling as we're clicking a different one
    renderer.selectPrevFromSelectionId(3);
    QCOMPARE(renderer.selectedItem(), 7);
    QCOMPARE(renderer.selectionLocked(), true);

    renderer.clearData();
    QCOMPARE(renderer.selectedItem(), -1);
    QCOMPARE(renderer.selectionLocked(), true);
}

QTEST_MAIN(tst_TimelineRenderer)

#include "tst_timelinerenderer.moc"
