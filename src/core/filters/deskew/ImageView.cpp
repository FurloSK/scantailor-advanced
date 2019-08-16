// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "ImageView.h"
#include <Constants.h>
#include <core/IconProvider.h>
#include <QAction>
#include <QPainter>
#include <QScrollBar>
#include <QStyle>
#include <QWheelEvent>
#include <boost/bind.hpp>
#include "ImagePresentation.h"

namespace deskew {
const double ImageView::m_maxRotationDeg = 45.0;
const double ImageView::m_maxRotationSin = std::sin(m_maxRotationDeg * constants::DEG2RAD);
const int ImageView::m_cellSize = 20;

ImageView::ImageView(const QImage& image, const QImage& downscaled_image, const ImageTransformation& xform)
    : ImageViewBase(image, downscaled_image, ImagePresentation(xform.transform(), xform.resultingPreCropArea())),
      m_handlePixmap(IconProvider::getInstance().getIcon("aqua-sphere").pixmap(16, 16)),
      m_dragHandler(*this),
      m_zoomHandler(*this),
      m_xform(xform) {
  setMouseTracking(true);

  interactionState().setDefaultStatusTip(tr("Use Ctrl+Wheel to rotate or Ctrl+Shift+Wheel for finer rotation."));

  const QString tip(tr("Drag this handle to rotate the image."));
  const double hit_radius = std::max<double>(0.5 * m_handlePixmap.width(), 15.0);
  for (int i = 0; i < 2; ++i) {
    m_handles[i].setHitRadius(hit_radius);
    m_handles[i].setPositionCallback(boost::bind(&ImageView::handlePosition, this, i));
    m_handles[i].setMoveRequestCallback(boost::bind(&ImageView::handleMoveRequest, this, i, _1));
    m_handles[i].setDragFinishedCallback(boost::bind(&ImageView::dragFinished, this));

    m_handleInteractors[i].setProximityStatusTip(tip);
    m_handleInteractors[i].setObject(&m_handles[i]);

    makeLastFollower(m_handleInteractors[i]);
  }

  m_zoomHandler.setFocus(ZoomHandler::CENTER);

  rootInteractionHandler().makeLastFollower(*this);
  rootInteractionHandler().makeLastFollower(m_dragHandler);
  rootInteractionHandler().makeLastFollower(m_zoomHandler);

  auto* rotateLeft = new QAction(nullptr);
  rotateLeft->setShortcut(QKeySequence(","));
  connect(rotateLeft, SIGNAL(triggered(bool)), SLOT(doRotateLeft()));
  addAction(rotateLeft);

  auto* rotateRight = new QAction(nullptr);
  rotateRight->setShortcut(QKeySequence("."));
  connect(rotateRight, SIGNAL(triggered(bool)), SLOT(doRotateRight()));
  addAction(rotateRight);
}

ImageView::~ImageView() = default;

void ImageView::doRotate(double deg) {
  manualDeskewAngleSetExternally(m_xform.postRotation() + deg);
  emit manualDeskewAngleSet(m_xform.postRotation());
}

void ImageView::doRotateLeft() {
  doRotate(-0.10);
}

void ImageView::doRotateRight() {
  doRotate(0.10);
}

void ImageView::manualDeskewAngleSetExternally(const double degrees) {
  if (m_xform.postRotation() == degrees) {
    return;
  }

  m_xform.setPostRotation(degrees);
  updateTransform(ImagePresentation(m_xform.transform(), m_xform.resultingPreCropArea()));
}

void ImageView::onPaint(QPainter& painter, const InteractionState& interaction) {
  painter.setWorldMatrixEnabled(false);
  painter.setRenderHints(QPainter::Antialiasing, false);

  const double w = maxViewportRect().width();
  const double h = maxViewportRect().height();
  const QPointF center(getImageRotationOrigin());

  // Draw the semi-transparent grid.
  QPen pen(QColor(0, 0, 0xd1, 90));
  pen.setCosmetic(true);
  pen.setWidth(1);
  painter.setPen(pen);
  QVector<QLineF> lines;
  for (double y = center.y(); (y -= m_cellSize) > 0.0;) {
    lines.push_back(QLineF(0.5, y, w - 0.5, y));
  }
  for (double y = center.y(); (y += m_cellSize) < h;) {
    lines.push_back(QLineF(0.5, y, w - 0.5, y));
  }
  for (double x = center.x(); (x -= m_cellSize) > 0.0;) {
    lines.push_back(QLineF(x, 0.5, x, h - 0.5));
  }
  for (double x = center.x(); (x += m_cellSize) < w;) {
    lines.push_back(QLineF(x, 0.5, x, h - 0.5));
  }
  painter.drawLines(lines);

  // Draw the horizontal and vertical line crossing at the center.
  pen.setColor(QColor(0, 0, 0xd1));
  painter.setPen(pen);
  painter.setBrush(Qt::NoBrush);
  painter.drawLine(QPointF(0.5, center.y()), QPointF(w - 0.5, center.y()));
  painter.drawLine(QPointF(center.x(), 0.5), QPointF(center.x(), h - 0.5));
  // Draw the rotation arcs.
  // Those will look like this (  )
  const QRectF arc_square(getRotationArcSquare());

  painter.setRenderHints(QPainter::Antialiasing, true);
  pen.setWidthF(1.5);
  painter.setPen(pen);
  painter.setBrush(Qt::NoBrush);
  painter.drawArc(arc_square, qRound(16 * -m_maxRotationDeg), qRound(16 * 2 * m_maxRotationDeg));
  painter.drawArc(arc_square, qRound(16 * (180 - m_maxRotationDeg)), qRound(16 * 2 * m_maxRotationDeg));

  const std::pair<QPointF, QPointF> handles(getRotationHandles(arc_square));

  QRectF rect(m_handlePixmap.rect());
  rect.moveCenter(handles.first);
  painter.drawPixmap(rect.topLeft(), m_handlePixmap);
  rect.moveCenter(handles.second);
  painter.drawPixmap(rect.topLeft(), m_handlePixmap);
}  // ImageView::onPaint

void ImageView::onWheelEvent(QWheelEvent* event, InteractionState& interaction) {
  if (interaction.captured()) {
    return;
  }

  double degree_fraction = 0;

  if (event->modifiers() == Qt::ControlModifier) {
    degree_fraction = 0.1;
  } else if (event->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier)) {
    degree_fraction = 0.05;
  } else {
    return;
  }

  event->accept();
  const double delta = degree_fraction * event->delta() / 120;
  double angle_deg = m_xform.postRotation() - delta;
  angle_deg = qBound(-m_maxRotationDeg, angle_deg, m_maxRotationDeg);
  if (angle_deg == m_xform.postRotation()) {
    return;
  }

  m_xform.setPostRotation(angle_deg);
  updateTransformPreservingScale(ImagePresentation(m_xform.transform(), m_xform.resultingPreCropArea()));
  emit manualDeskewAngleSet(m_xform.postRotation());
}  // ImageView::onWheelEvent

QPointF ImageView::handlePosition(int idx) const {
  const std::pair<QPointF, QPointF> handles(getRotationHandles(getRotationArcSquare()));
  if (idx == 0) {
    return handles.first;
  } else {
    return handles.second;
  }
}

void ImageView::handleMoveRequest(int idx, const QPointF& pos) {
  const QRectF arc_square(getRotationArcSquare());
  const double arc_radius = 0.5 * arc_square.width();
  const double abs_y = pos.y();
  double rel_y = abs_y - arc_square.center().y();
  rel_y = qBound(-arc_radius, rel_y, arc_radius);

  double angle_rad = std::asin(rel_y / arc_radius);
  if (idx == 0) {
    angle_rad = -angle_rad;
  }
  double angle_deg = angle_rad * constants::RAD2DEG;
  angle_deg = qBound(-m_maxRotationDeg, angle_deg, m_maxRotationDeg);
  if (angle_deg == m_xform.postRotation()) {
    return;
  }

  m_xform.setPostRotation(angle_deg);
  updateTransformPreservingScale(ImagePresentation(m_xform.transform(), m_xform.resultingPreCropArea()));
}

void ImageView::dragFinished() {
  emit manualDeskewAngleSet(m_xform.postRotation());
}

/**
 * Get the point at the center of the widget, in widget coordinates.
 * The point may be adjusted to to ensure it's at the center of a pixel.
 */
QPointF ImageView::getImageRotationOrigin() const {
  const QRectF viewport_rect(maxViewportRect());

  return QPointF(std::floor(0.5 * viewport_rect.width()) + 0.5, std::floor(0.5 * viewport_rect.height()) + 0.5);
}

/**
 * Get the square in widget coordinates where two rotation arcs will be drawn.
 */
QRectF ImageView::getRotationArcSquare() const {
  const double h_margin
      = 0.5 * m_handlePixmap.width()
        + verticalScrollBar()->style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, verticalScrollBar());
  const double v_margin
      = 0.5 * m_handlePixmap.height()
        + horizontalScrollBar()->style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, horizontalScrollBar());

  QRectF reduced_screen_rect(maxViewportRect());
  reduced_screen_rect.adjust(h_margin, v_margin, -h_margin, -v_margin);

  QSizeF arc_size(1.0, m_maxRotationSin);
  arc_size.scale(reduced_screen_rect.size(), Qt::KeepAspectRatio);
  arc_size.setHeight(arc_size.width());

  QRectF arc_square(QPointF(0, 0), arc_size);
  arc_square.moveCenter(reduced_screen_rect.center());

  return arc_square;
}

std::pair<QPointF, QPointF> ImageView::getRotationHandles(const QRectF& arc_square) const {
  const double rot_sin = m_xform.postRotationSin();
  const double rot_cos = m_xform.postRotationCos();
  const double arc_radius = 0.5 * arc_square.width();
  const QPointF arc_center(arc_square.center());
  QPointF left_handle(-rot_cos * arc_radius, -rot_sin * arc_radius);
  left_handle += arc_center;
  QPointF right_handle(rot_cos * arc_radius, rot_sin * arc_radius);
  right_handle += arc_center;

  return std::make_pair(left_handle, right_handle);
}
}  // namespace deskew