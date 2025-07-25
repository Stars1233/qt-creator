// Copyright (C) 2016 Jochen Becher
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "classitem.h"

#include "qmt/controller/namecontroller.h"
#include "qmt/diagram/dclass.h"
#include "qmt/diagram/dinheritance.h"
#include "qmt/diagram_controller/diagramcontroller.h"
#include "qmt/diagram_scene/diagramsceneconstants.h"
#include "qmt/diagram_scene/diagramscenemodel.h"
#include "qmt/diagram_scene/parts/contextlabelitem.h"
#include "qmt/diagram_scene/parts/customiconitem.h"
#include "qmt/diagram_scene/parts/editabletextitem.h"
#include "qmt/diagram_scene/parts/relationstarter.h"
#include "qmt/diagram_scene/parts/stereotypesitem.h"
#include "qmt/diagram_scene/parts/templateparameterbox.h"
#include "qmt/infrastructure/contextmenuaction.h"
#include "qmt/infrastructure/geometryutilities.h"
#include "qmt/infrastructure/qmtassert.h"
#include "qmt/model/mclass.h"
#include "qmt/model/mclassmember.h"
#include "qmt/model/massociation.h"
#include "qmt/model/minheritance.h"
#include "qmt/model_controller/modelcontroller.h"
#include "qmt/stereotype/stereotypecontroller.h"
#include "qmt/stereotype/stereotypeicon.h"
#include "qmt/style/stylecontroller.h"
#include "qmt/style/style.h"
#include "qmt/tasks/diagramscenecontroller.h"
#include "qmt/tasks/ielementtasks.h"

#include <utils/algorithm.h>

#include <QGraphicsScene>
#include <QGraphicsRectItem>
#include <QGraphicsSimpleTextItem>
#include <QGraphicsLineItem>
#include <QBrush>
#include <QPen>
#include <QFont>
#include <QMenu>

#include <algorithm>

#include <qmt/stereotype/customrelation.h>

#include "../../modelinglibtr.h"

namespace qmt {

static const char ASSOCIATION[] = "association";
static const char INHERITANCE[] = "inheritance";

static const qreal MINIMUM_AUTO_WIDTH = 80.0;
static const qreal MINIMUM_AUTO_HEIGHT = 60.0;
static const qreal BODY_VERT_BORDER = 4.0;
static const qreal BODY_HORIZ_BORDER = 4.0;
static const qreal BODY_HORIZ_DISTANCE = 4.0;

ClassItem::ClassItem(DClass *klass, DiagramSceneModel *diagramSceneModel, QGraphicsItem *parent)
    : ObjectItem("class", klass, diagramSceneModel, parent)
{
}

ClassItem::~ClassItem()
{
}

void ClassItem::update()
{
    prepareGeometryChange();

    updateStereotypeIconDisplay();

    auto diagramClass = dynamic_cast<DClass *>(object());
    QMT_ASSERT(diagramClass, return);

    const Style *style = adaptedStyle(stereotypeIconId());

    if (diagramClass->showAllMembers()) {
        updateMembers(style);
    } else {
        m_attributesText.clear();
        m_methodsText.clear();
    }

    updateCustomIcon(style);

    // shape
    if (!customIconItem()) {
        if (!m_shape)
            m_shape = new QGraphicsRectItem(this);
        m_shape->setBrush(style->fillBrush());
        m_shape->setPen(style->outerLinePen());
        m_shape->setZValue(SHAPE_ZVALUE);
    } else if (m_shape) {
        m_shape->scene()->removeItem(m_shape);
        delete m_shape;
        m_shape = nullptr;
    }

    // stereotypes
    updateStereotypes(stereotypeIconId(), stereotypeIconDisplay(), style);

    // base classes
    DiagramController *diagramController = diagramSceneModel()->diagramController();
    ModelController *modelController = diagramController->modelController();
    MClass *klass = modelController->findElement<MClass>(diagramClass->modelUid());
    if (klass) {
        // TODO cache relation's Uids and check for change
        QList<QString> baseClasses;
        for (const auto &handle : klass->relations()) {
            if (MInheritance *mInheritance = dynamic_cast<MInheritance *>(handle.target())) {
                if (mInheritance->base() != klass->uid()) {
                    DInheritance *dInheritance = diagramController->findDelegate<DInheritance>(mInheritance, diagramSceneModel()->diagram());
                    if (dInheritance) {
                        DClass *dBaseClass = diagramController->findElement<DClass>(dInheritance->base(), diagramSceneModel()->diagram());
                        if (dBaseClass)
                            continue;
                    }
                    MClass *mBaseClass = diagramController->modelController()->findElement<MClass>(mInheritance->base());
                    if (mBaseClass) {
                        QString baseClassName;
                        baseClassName += mBaseClass->name();
                        baseClasses.append(baseClassName);
                    }
                }
            }
        }
        if (!baseClasses.isEmpty()) {
            if (!m_baseClasses)
                m_baseClasses = new QGraphicsSimpleTextItem(this);
            QFont font = style->smallFont();
            font.setItalic(true);
            m_baseClasses->setFont(font);
            m_baseClasses->setBrush(style->textBrush());
            m_baseClasses->setText(baseClasses.join('\n'));
        } else if (m_baseClasses) {
            if (m_baseClasses->scene())
                m_baseClasses->scene()->removeItem(m_baseClasses);
            delete m_baseClasses;
            m_baseClasses = nullptr;
        }
    }

    // namespace
    if (!suppressTextDisplay() && !diagramClass->umlNamespace().isEmpty()) {
        if (!m_namespace)
            m_namespace = new QGraphicsSimpleTextItem(this);
        m_namespace->setFont(style->smallFont());
        m_namespace->setBrush(style->textBrush());
        m_namespace->setText(diagramClass->umlNamespace());
    } else if (m_namespace) {
        m_namespace->scene()->removeItem(m_namespace);
        delete m_namespace;
        m_namespace = nullptr;
    }

    // class name
    updateNameItem(style);

    // context
    if (!suppressTextDisplay() && showContext()) {
        if (!m_contextLabel)
            m_contextLabel = new ContextLabelItem(this);
        m_contextLabel->setFont(style->smallFont());
        m_contextLabel->setBrush(style->textBrush());
        m_contextLabel->setContext(object()->context());
    } else if (m_contextLabel) {
        m_contextLabel->scene()->removeItem(m_contextLabel);
        delete m_contextLabel;
        m_contextLabel = nullptr;
    }

    // attributes separator
    if (m_shape || (!suppressTextDisplay() && (!m_attributesText.isEmpty() || !m_methodsText.isEmpty()))) {
        if (!m_attributesSeparator)
            m_attributesSeparator = new QGraphicsLineItem(this);
        m_attributesSeparator->setPen(style->innerLinePen());
        m_attributesSeparator->setZValue(SHAPE_DETAILS_ZVALUE);
    } else if (m_attributesSeparator) {
        m_attributesSeparator->scene()->removeItem(m_attributesSeparator);
        delete m_attributesSeparator;
        m_attributesSeparator = nullptr;
    }

    // attributes
    if (!suppressTextDisplay() && !m_attributesText.isEmpty()) {
        if (!m_attributes)
            m_attributes = new QGraphicsTextItem(this);
        m_attributes->setFont(style->normalFont());
        //m_attributes->setBrush(style->textBrush());
        m_attributes->setDefaultTextColor(style->textBrush().color());
        m_attributes->setHtml(m_attributesText);
    } else if (m_attributes) {
        m_attributes->scene()->removeItem(m_attributes);
        delete m_attributes;
        m_attributes = nullptr;
    }

    // methods separator
    if (m_shape || (!suppressTextDisplay() && (!m_attributesText.isEmpty() || !m_methodsText.isEmpty()))) {
        if (!m_methodsSeparator)
            m_methodsSeparator = new QGraphicsLineItem(this);
        m_methodsSeparator->setPen(style->innerLinePen());
        m_methodsSeparator->setZValue(SHAPE_DETAILS_ZVALUE);
    } else if (m_methodsSeparator) {
        m_methodsSeparator->scene()->removeItem(m_methodsSeparator);
        delete m_methodsSeparator;
        m_methodsSeparator = nullptr;
    }

    // methods
    if (!suppressTextDisplay() && !m_methodsText.isEmpty()) {
        if (!m_methods)
            m_methods = new QGraphicsTextItem(this);
        m_methods->setFont(style->normalFont());
        //m_methods->setBrush(style->textBrush());
        m_methods->setDefaultTextColor(style->textBrush().color());
        m_methods->setHtml(m_methodsText);
    } else if (m_methods) {
        m_methods->scene()->removeItem(m_methods);
        delete m_methods;
        m_methods = nullptr;
    }

    // template parameters
    if (templateDisplay() == DClass::TemplateBox && !diagramClass->templateParameters().isEmpty()) {
        // TODO due to a bug in Qt the m_nameItem may get focus back when this item is newly created
        // 1. Select name item of class without template
        // 2. Click into template property (item name loses focus) and enter a letter
        // 3. Template box is created which gives surprisingly focus back to item name
        if (!m_templateParameterBox)
            m_templateParameterBox = new TemplateParameterBox(this);
        QPen pen = style->outerLinePen();
        pen.setStyle(Qt::DashLine);
        m_templateParameterBox->setPen(pen);
        m_templateParameterBox->setBrush(QBrush(Qt::white));
        m_templateParameterBox->setFont(style->smallFont());
        m_templateParameterBox->setTextBrush(style->textBrush());
        m_templateParameterBox->setTemplateParameters(diagramClass->templateParameters());
    } else if (m_templateParameterBox) {
        m_templateParameterBox->scene()->removeItem(m_templateParameterBox);
        delete m_templateParameterBox;
        m_templateParameterBox = nullptr;
    }

    updateSelectionMarker(customIconItem());
    updateRelationStarter();
    updateAlignmentButtons();
    updateGeometry();
}

bool ClassItem::intersectShapeWithLine(const QLineF &line, QPointF *intersectionPoint, QLineF *intersectionLine) const
{
    if (customIconItem()) {
        QList<QPolygonF> polygons = customIconItem()->outline();
        for (int i = 0; i < polygons.size(); ++i)
            polygons[i].translate(object()->pos() + object()->rect().topLeft());
        if (shapeIcon().textAlignment() == qmt::StereotypeIcon::TextalignBelow) {
            if (nameItem()) {
                QPolygonF polygon(nameItem()->boundingRect());
                polygon.translate(object()->pos() + nameItem()->pos());
                polygons.append(polygon);
            }
            if (m_contextLabel) {
                QPolygonF polygon(m_contextLabel->boundingRect());
                polygon.translate(object()->pos() + m_contextLabel->pos());
                polygons.append(polygon);
            }
        }
        return GeometryUtilities::intersect(polygons, line, nullptr, intersectionPoint, intersectionLine);
    }
    QPolygonF polygon;
    QRectF rect = object()->rect();
    rect.translate(object()->pos());
    polygon << rect.topLeft() << rect.topRight() << rect.bottomRight() << rect.bottomLeft() << rect.topLeft();
    return GeometryUtilities::intersect(polygon, line, intersectionPoint, intersectionLine);
}

QSizeF ClassItem::minimumSize() const
{
    return calcMinimumGeometry();
}

void ClassItem::relationDrawn(const QString &id, ObjectItem *targetItem, const QList<QPointF> &intermediatePoints)
{
    DiagramSceneController *diagramSceneController = diagramSceneModel()->diagramSceneController();
    if (id == INHERITANCE) {
        auto baseClass = dynamic_cast<DClass *>(targetItem->object());
        if (baseClass) {
            auto derivedClass = dynamic_cast<DClass *>(object());
            QMT_ASSERT(derivedClass, return);
            diagramSceneController->createInheritance(derivedClass, baseClass, intermediatePoints, diagramSceneModel()->diagram());
        }
        return;
    } else if (id == ASSOCIATION) {
        auto associatedClass = dynamic_cast<DClass *>(targetItem->object());
        if (associatedClass) {
            auto derivedClass = dynamic_cast<DClass *>(object());
            QMT_ASSERT(derivedClass, return);
            diagramSceneController->createAssociation(derivedClass, associatedClass, intermediatePoints, diagramSceneModel()->diagram());
        }
        return;
    } else {
        StereotypeController *stereotypeController = diagramSceneModel()->stereotypeController();
        CustomRelation customRelation = stereotypeController->findCustomRelation(id);
        if (!customRelation.isNull()) {
            switch (customRelation.element()) {
            case CustomRelation::Element::Inheritance:
            {
                auto baseClass = dynamic_cast<DClass *>(targetItem->object());
                if (baseClass) {
                    auto derivedClass = dynamic_cast<DClass *>(object());
                    QMT_ASSERT(derivedClass, return);
                    diagramSceneController->createInheritance(derivedClass, baseClass, intermediatePoints, diagramSceneModel()->diagram());
                }
                return;
            }
            case CustomRelation::Element::Association:
            {
                auto assoziatedClass = dynamic_cast<DClass *>(targetItem->object());
                if (assoziatedClass) {
                    auto derivedClass = dynamic_cast<DClass *>(object());
                    QMT_ASSERT(derivedClass, return);
                    diagramSceneController->createAssociation(derivedClass, assoziatedClass,
                                intermediatePoints, diagramSceneModel()->diagram(),
                                [diagramSceneController, customRelation]
                                (MAssociation *mAssociation, DAssociation *dAssociation) {
                        if (mAssociation && dAssociation) {
                            static const QHash<CustomRelation::Relationship, MAssociationEnd::Kind> relationship2KindMap = {
                                { CustomRelation::Relationship::Association, MAssociationEnd::Association },
                                { CustomRelation::Relationship::Aggregation, MAssociationEnd::Aggregation },
                                { CustomRelation::Relationship::Composition, MAssociationEnd::Composition } };
                            diagramSceneController->modelController()->startUpdateRelation(mAssociation);
                            mAssociation->setStereotypes(Utils::toList(customRelation.stereotypes()));
                            mAssociation->setName(customRelation.name());
                            MAssociationEnd endA;
                            endA.setCardinality(customRelation.endA().cardinality());
                            endA.setKind(relationship2KindMap.value(customRelation.endA().relationship()));
                            endA.setName(customRelation.endA().role());
                            endA.setNavigable(customRelation.endA().navigable());
                            mAssociation->setEndA(endA);
                            MAssociationEnd endB;
                            endB.setCardinality(customRelation.endB().cardinality());
                            endB.setKind(relationship2KindMap.value(customRelation.endB().relationship()));
                            endB.setName(customRelation.endB().role());
                            endB.setNavigable(customRelation.endB().navigable());
                            mAssociation->setEndB(endB);
                            diagramSceneController->modelController()->finishUpdateRelation(mAssociation, false);
                        }
                    });
                }
                return;
            }
            case CustomRelation::Element::Dependency:
            case CustomRelation::Element::Relation:
                // fall thru
                break;
            }
        }
    }
    ObjectItem::relationDrawn(id, targetItem, intermediatePoints);
}

bool ClassItem::extendContextMenu(QMenu *menu)
{
    bool extended = false;
    if (diagramSceneModel()->diagramSceneController()->elementTasks()->hasClassDefinition(object(), diagramSceneModel()->diagram())) {
        menu->addAction(new ContextMenuAction(Tr::tr("Show Definition"), "showDefinition", menu));
        extended = true;
    }
    return extended;
}

bool ClassItem::handleSelectedContextMenuAction(const QString &id)
{
    if (id == "showDefinition") {
        diagramSceneModel()->diagramSceneController()->elementTasks()->openClassDefinition(object(), diagramSceneModel()->diagram());
        return true;
    }
    return false;
}

QString ClassItem::buildDisplayName() const
{
    auto diagramClass = dynamic_cast<DClass *>(object());
    QMT_ASSERT(diagramClass, return QString());

    QString name;
    if (templateDisplay() == DClass::TemplateName && !diagramClass->templateParameters().isEmpty()) {
        name = object()->name();
        name += QLatin1Char('<');
        bool first = true;
        for (const QString &p : diagramClass->templateParameters()) {
            if (!first)
                name += QLatin1Char(',');
            name += p;
            first = false;
        }
        name += QLatin1Char('>');
    } else {
        name = object()->name();
    }
    return name;
}

void ClassItem::setFromDisplayName(const QString &displayName)
{
    if (templateDisplay() == DClass::TemplateName) {
        QString name;
        QStringList templateParameters;
        // NOTE namespace is ignored because it has its own edit field
        if (NameController::parseClassName(displayName, nullptr, &name, &templateParameters)) {
            auto diagramClass = dynamic_cast<DClass *>(object());
            QMT_ASSERT(diagramClass, return);
            ModelController *modelController = diagramSceneModel()->diagramSceneController()->modelController();
            MClass *mklass = modelController->findObject<MClass>(diagramClass->modelUid());
            if (mklass && (name != mklass->name() || templateParameters != mklass->templateParameters())) {
                modelController->startUpdateObject(mklass);
                mklass->setName(name);
                mklass->setTemplateParameters(templateParameters);
                modelController->finishUpdateObject(mklass, false);
            }
        }
    } else {
        ObjectItem::setFromDisplayName(displayName);
    }
}

void ClassItem::addRelationStarterTool(const QString &id)
{
    if (id == INHERITANCE)
        relationStarter()->addArrow(INHERITANCE, ArrowItem::ShaftSolid,
                                    ArrowItem::HeadNone, ArrowItem::HeadTriangle,
                                    Tr::tr("Inheritance"));
    else if (id == ASSOCIATION)
        relationStarter()->addArrow(ASSOCIATION, ArrowItem::ShaftSolid,
                                    ArrowItem::HeadNone, ArrowItem::HeadFilledTriangle,
                                    Tr::tr("Association"));
    else
        ObjectItem::addRelationStarterTool(id);
}

void ClassItem::addRelationStarterTool(const CustomRelation &customRelation)
{
    ArrowItem::Shaft shaft = ArrowItem::ShaftSolid;
    ArrowItem::Head headStart = ArrowItem::HeadNone;
    ArrowItem::Head headEnd = ArrowItem::HeadNone;
    switch (customRelation.element()) {
    case CustomRelation::Element::Inheritance:
        shaft = ArrowItem::ShaftSolid;
        headEnd = ArrowItem::HeadTriangle;
        break;
    case CustomRelation::Element::Association:
        switch (customRelation.endA().relationship()) {
        case CustomRelation::Relationship::Association:
            if (customRelation.endA().navigable() && customRelation.endB().navigable()) {
                headStart = ArrowItem::HeadNone;
                headEnd = ArrowItem::HeadNone;
            } else if (customRelation.endA().navigable()) {
                headStart = ArrowItem::HeadFilledTriangle;
            } else {
                headEnd = ArrowItem::HeadFilledTriangle;
            }
            break;
        case CustomRelation::Relationship::Aggregation:
            headStart = ArrowItem::HeadDiamond;
            break;
        case CustomRelation::Relationship::Composition:
            headStart = ArrowItem::HeadFilledDiamond;
            break;
        }
        break;
    case CustomRelation::Element::Dependency:
    case CustomRelation::Element::Relation:
        ObjectItem::addRelationStarterTool(customRelation);
        return;
    }
    relationStarter()->addArrow(customRelation.id(), shaft, headStart, headEnd,
                                customRelation.title());
}

void ClassItem::addStandardRelationStarterTools()
{
    ObjectItem::addStandardRelationStarterTools();
    addRelationStarterTool(INHERITANCE);
    addRelationStarterTool(ASSOCIATION);
}

DClass::TemplateDisplay ClassItem::templateDisplay() const
{
    auto diagramClass = dynamic_cast<DClass *>(object());
    QMT_ASSERT(diagramClass, return DClass::TemplateSmart);

    DClass::TemplateDisplay templateDisplay = diagramClass->templateDisplay();
    if (templateDisplay == DClass::TemplateSmart) {
        if (customIconItem())
            templateDisplay = DClass::TemplateName;
        else
            templateDisplay = DClass::TemplateBox;
    }
    return templateDisplay;
}

QSizeF ClassItem::calcMinimumGeometry() const
{
    double width = 0.0;
    double height = 0.0;

    if (customIconItem()) {
        QSizeF sz = customIconItemMinimumSize(customIconItem(),
                                              CUSTOM_ICON_MINIMUM_AUTO_WIDTH, CUSTOM_ICON_MINIMUM_AUTO_HEIGHT);
        if (shapeIcon().textAlignment() != qmt::StereotypeIcon::TextalignTop
                && shapeIcon().textAlignment() != qmt::StereotypeIcon::TextalignCenter)
            return sz;
        width = sz.width();
    }

    height += BODY_VERT_BORDER;
    double top_row_width = BODY_HORIZ_DISTANCE;
    double top_row_height = 0;
    if (CustomIconItem *stereotypeIconItem = this->stereotypeIconItem()) {
        top_row_width += stereotypeIconItem->boundingRect().width();
        top_row_height = stereotypeIconItem->boundingRect().height();
    }
    if (m_baseClasses) {
        top_row_width += m_baseClasses->boundingRect().width();
        top_row_height = std::max(top_row_height, m_baseClasses->boundingRect().height());
    }
    width = std::max(width, top_row_width);
    height += top_row_height;
    if (StereotypesItem *stereotypesItem = this->stereotypesItem()) {
        width = std::max(width, stereotypesItem->boundingRect().width());
        height += stereotypesItem->boundingRect().height();
    }
    if (m_namespace) {
        width = std::max(width, m_namespace->boundingRect().width());
        height += m_namespace->boundingRect().height();
    }
    if (nameItem()) {
        width = std::max(width, nameItem()->boundingRect().width());
        height += nameItem()->boundingRect().height();
    }
    if (m_contextLabel)
        height += m_contextLabel->height();
    if (m_attributesSeparator)
        height += 8.0;
    if (m_attributes) {
        width = std::max(width, m_attributes->boundingRect().width());
        height += m_attributes->boundingRect().height();
    }
    if (m_methodsSeparator)
        height += 8.0;
    if (m_methods) {
        width = std::max(width, m_methods->boundingRect().width());
        height += m_methods->boundingRect().height();
    }
    height += BODY_VERT_BORDER;

    width = BODY_HORIZ_BORDER + width + BODY_HORIZ_BORDER;

    return GeometryUtilities::ensureMinimumRasterSize(QSizeF(width, height), 2 * RASTER_WIDTH, 2 * RASTER_HEIGHT);
}

void ClassItem::updateGeometry()
{
    prepareGeometryChange();

    // calc width and height
    double width = 0.0;
    double height = 0.0;

    QSizeF geometry = calcMinimumGeometry();
    width = geometry.width();
    height = geometry.height();

    if (object()->isAutoSized()) {
        correctAutoSize(customIconItem(), width, height, MINIMUM_AUTO_WIDTH, MINIMUM_AUTO_HEIGHT);
    } else {
        QRectF rect = object()->rect();
        if (rect.width() > width)
            width = rect.width();
        if (rect.height() > height)
            height = rect.height();
    }

    // update sizes and positions
    double left = -width / 2.0;
    double right = width / 2.0;
    double top = -height / 2.0;
    //double bottom = height / 2.0;
    double y = top;

    setPos(object()->pos());

    QRectF rect(left, top, width, height);

    // the object is updated without calling DiagramController intentionally.
    // attribute rect is not a real attribute stored on DObject but
    // a backup for the graphics item used for manual resized and persistency.
    object()->setRect(rect);

    if (customIconItem()) {
        customIconItem()->setPos(left, top);
        customIconItem()->setActualSize(QSizeF(width, height));
    }

    if (m_shape)
        m_shape->setRect(rect);

    if (customIconItem()) {
        switch (shapeIcon().textAlignment()) {
        case qmt::StereotypeIcon::TextalignBelow:
            y += height + BODY_VERT_BORDER;
            break;
        case qmt::StereotypeIcon::TextalignCenter:
        {
            double h = 0.0;
            if (CustomIconItem *stereotypeIconItem = this->stereotypeIconItem())
                h += stereotypeIconItem->boundingRect().height();
            if (StereotypesItem *stereotypesItem = this->stereotypesItem())
                h += stereotypesItem->boundingRect().height();
            if (nameItem())
                h += nameItem()->boundingRect().height();
            if (m_contextLabel)
                h += m_contextLabel->height();
            y = top + (height - h) / 2.0;
            break;
        }
        case qmt::StereotypeIcon::TextalignNone:
            // nothing to do
            break;
        case qmt::StereotypeIcon::TextalignTop:
            y += BODY_VERT_BORDER;
            break;
        }
    } else {
        y += BODY_VERT_BORDER;
    }
    double first_row_height = 0;
    if (m_baseClasses) {
        m_baseClasses->setPos(left + BODY_HORIZ_BORDER, y);
        first_row_height = m_baseClasses->boundingRect().height();
    }
    if (CustomIconItem *stereotypeIconItem = this->stereotypeIconItem()) {
        stereotypeIconItem->setPos(right - stereotypeIconItem->boundingRect().width() - BODY_HORIZ_BORDER, y);
        first_row_height = std::max(first_row_height, stereotypeIconItem->boundingRect().height());
    }
    y += first_row_height;
    if (StereotypesItem *stereotypesItem = this->stereotypesItem()) {
        stereotypesItem->setPos(-stereotypesItem->boundingRect().width() / 2.0, y);
        y += stereotypesItem->boundingRect().height();
    }
    if (m_namespace) {
        m_namespace->setPos(-m_namespace->boundingRect().width() / 2.0, y);
        y += m_namespace->boundingRect().height();
    }
    if (nameItem()) {
        nameItem()->setPos(-nameItem()->boundingRect().width() / 2.0, y);
        y += nameItem()->boundingRect().height();
    }
    if (m_contextLabel) {
        if (customIconItem())
            m_contextLabel->resetMaxWidth();
        else
            m_contextLabel->setMaxWidth(width - 2 * BODY_HORIZ_BORDER);
        m_contextLabel->setPos(-m_contextLabel->boundingRect().width() / 2.0, y);
        y += m_contextLabel->boundingRect().height();
    }
    if (m_attributesSeparator) {
        m_attributesSeparator->setLine(left, 4.0, right, 4.0);
        m_attributesSeparator->setPos(0, y);
        y += 8.0;
    }
    if (m_attributes) {
        if (customIconItem())
            m_attributes->setPos(-m_attributes->boundingRect().width() / 2.0, y);
        else
            m_attributes->setPos(left + BODY_HORIZ_BORDER, y);
        y += m_attributes->boundingRect().height();
    }
    if (m_methodsSeparator) {
        m_methodsSeparator->setLine(left, 4.0, right, 4.0);
        m_methodsSeparator->setPos(0, y);
        y += 8.0;
    }
    if (m_methods) {
        if (customIconItem())
            m_methods->setPos(-m_methods->boundingRect().width() / 2.0, y);
        else
            m_methods->setPos(left + BODY_HORIZ_BORDER, y);
        y += m_methods->boundingRect().height();
    }

    if (m_templateParameterBox) {
        m_templateParameterBox->setBreakLines(false);
        double x = right - m_templateParameterBox->boundingRect().width() * 0.8;
        if (x < 0) {
            m_templateParameterBox->setBreakLines(true);
            x = right - m_templateParameterBox->boundingRect().width() * 0.8;
        }
        if (x < 0)
            x = 0;
        m_templateParameterBox->setPos(x, top - m_templateParameterBox->boundingRect().height() + BODY_VERT_BORDER);
    }

    updateSelectionMarkerGeometry(rect);
    updateRelationStarterGeometry(rect);
    updateAlignmentButtonsGeometry(rect);
    updateDepth();
}

void ClassItem::updateMembers(const Style *style)
{
    Q_UNUSED(style)

    m_attributesText.clear();
    m_methodsText.clear();

    MClassMember::Visibility attributesVisibility = MClassMember::VisibilityUndefined;
    MClassMember::Visibility methodsVisibility = MClassMember::VisibilityUndefined;
    QString attributesGroup;
    QString methodsGroup;

    MClassMember::Visibility *currentVisibility = nullptr;
    QString *currentGroup = nullptr;
    QString *text = nullptr;

    auto dclass = dynamic_cast<DClass *>(object());
    QMT_ASSERT(dclass, return);

    for (const MClassMember &member : dclass->members()) {
        switch (member.memberType()) {
        case MClassMember::MemberUndefined:
            QMT_ASSERT(false, return);
            break;
        case MClassMember::MemberAttribute:
            currentVisibility = &attributesVisibility;
            currentGroup = &attributesGroup;
            text = &m_attributesText;
            break;
        case MClassMember::MemberMethod:
            currentVisibility = &methodsVisibility;
            currentGroup = &methodsGroup;
            text = &m_methodsText;
            break;
        }

        bool needBr = false;
        if (text && !text->isEmpty())
            needBr = true;

        if (currentVisibility)
            *currentVisibility = member.visibility();
        if (currentGroup && member.group() != *currentGroup) {
            needBr = false;
            *text += QString("<p style=\"padding:0;margin-top:6;margin-bottom:0;\"><b>[%1]</b></p>").arg(member.group());
            *currentGroup = member.group();
        }

        if (needBr)
            *text += "<br/>";

        bool addSpace = false;
        bool haveSignal = false;
        bool haveSlot = false;
        if (member.visibility() != MClassMember::VisibilityUndefined) {
            QString vis;
            switch (member.visibility()) {
            case MClassMember::VisibilityUndefined:
                break;
            case MClassMember::VisibilityPublic:
                vis = "+";
                addSpace = true;
                break;
            case MClassMember::VisibilityProtected:
                vis = "#";
                addSpace = true;
                break;
            case MClassMember::VisibilityPrivate:
                vis = "-";
                addSpace = true;
                break;
            case MClassMember::VisibilitySignals:
                vis = "&gt;";
                haveSignal = true;
                addSpace = true;
                break;
            case MClassMember::VisibilityPrivateSlots:
                vis = "-$";
                haveSlot = true;
                addSpace = true;
                break;
            case MClassMember::VisibilityProtectedSlots:
                vis = "#$";
                haveSlot = true;
                addSpace = true;
                break;
            case MClassMember::VisibilityPublicSlots:
                vis = "+$";
                haveSlot = true;
                addSpace = true;
                break;
            }
            *text += vis;
        }

        if (member.properties() & MClassMember::PropertyQsignal && !haveSignal) {
            *text += "&gt;";
            addSpace = true;
        }
        if (member.properties() & MClassMember::PropertyQslot && !haveSlot) {
            *text += "$";
            addSpace = true;
        }
        if (addSpace)
            *text += " ";
        if (member.properties() & MClassMember::PropertyQinvokable)
            *text += "invokable ";
        if (!member.stereotypes().isEmpty()) {
            *text += StereotypesItem::format(member.stereotypes());
            *text += " ";
        }
        if (member.properties() & MClassMember::PropertyStatic)
            *text += "static ";
        if (member.properties() & MClassMember::PropertyVirtual)
            *text += "virtual ";
        *text += member.declaration().toHtmlEscaped();
        if (member.properties() & MClassMember::PropertyConst)
            *text += " const";
        if (member.properties() & MClassMember::PropertyOverride)
            *text += " override";
        if (member.properties() & MClassMember::PropertyFinal)
            *text += " final";
        if (member.properties() & MClassMember::PropertyAbstract)
            *text += " = 0";
    }
}

} // namespace qmt
