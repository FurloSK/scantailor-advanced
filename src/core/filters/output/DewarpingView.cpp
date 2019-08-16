// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "DewarpingView.h"
#include <QDebug>
#include <QPainter>
#include <boost/bind.hpp>
#include <boost/foreach.hpp>
#include "ImagePresentation.h"
#include "ToLineProjector.h"
#include <CylindricalSurfaceDewarper.h>
#include <Constants.h>
#include "spfit/ConstraintSet.h"
#include "spfit/LinearForceBalancer.h"
#include "spfit/PolylineModelShape.h"
#include "spfit/SplineFitter.h"

namespace output {
DewarpingView::DewarpingView(const QImage& image,
                             const ImagePixmapUnion& downscaled_image,
                             const QTransform& image_to_virt,
                             const QPolygonF& virt_display_area,
                             const QRectF& virt_content_rect,
                             const PageId& page_id,
                             DewarpingOptions dewarping_options,
                             const dewarping::DistortionModel& distortion_model,
                             const DepthPerception& depth_perception)
    : ImageViewBase(image, downscaled_image, ImagePresentation(image_to_virt, virt_display_area)),
      m_pageId(page_id),
      m_virtDisplayArea(virt_display_area),
      m_dewarpingOptions(dewarping_options),
      m_distortionModel(distortion_model),
      m_depthPerception(depth_perception),
      m_dragHandler(*this),
      m_zoomHandler(*this) {
  setMouseTracking(true);

  const QPolygonF source_content_rect(virtualToImage().map(virt_content_rect));

  XSpline top_spline(m_distortionModel.topCurve().xspline());
  XSpline bottom_spline(m_distortionModel.bottomCurve().xspline());
  if (top_spline.numControlPoints() < 2) {
    const std::vector<QPointF>& polyline = m_distortionModel.topCurve().polyline();

    XSpline new_top_spline;
    if (polyline.size() < 2) {
      initNewSpline(new_top_spline, source_content_rect[0], source_content_rect[1], &dewarping_options);
    } else {
      initNewSpline(new_top_spline, polyline.front(), polyline.back(), &dewarping_options);
      fitSpline(new_top_spline, polyline);
    }

    top_spline.swap(new_top_spline);
  }
  if (bottom_spline.numControlPoints() < 2) {
    const std::vector<QPointF>& polyline = m_distortionModel.bottomCurve().polyline();

    XSpline new_bottom_spline;
    if (polyline.size() < 2) {
      initNewSpline(new_bottom_spline, source_content_rect[3], source_content_rect[2], &dewarping_options);
    } else {
      initNewSpline(new_bottom_spline, polyline.front(), polyline.back(), &dewarping_options);
      fitSpline(new_bottom_spline, polyline);
    }

    bottom_spline.swap(new_bottom_spline);
  }

  m_topSpline.setSpline(top_spline);
  m_bottomSpline.setSpline(bottom_spline);

  InteractiveXSpline* splines[2] = {&m_topSpline, &m_bottomSpline};
  int curve_idx = -1;
  for (InteractiveXSpline* spline : splines) {
    ++curve_idx;
    spline->setModifiedCallback(boost::bind(&DewarpingView::curveModified, this, curve_idx));
    spline->setDragFinishedCallback(boost::bind(&DewarpingView::dragFinished, this));
    spline->setStorageTransform(boost::bind(&DewarpingView::sourceToWidget, this, _1),
                                boost::bind(&DewarpingView::widgetToSource, this, _1));
    makeLastFollower(*spline);
  }

  m_distortionModel.setTopCurve(dewarping::Curve(m_topSpline.spline()));
  m_distortionModel.setBottomCurve(dewarping::Curve(m_bottomSpline.spline()));

  rootInteractionHandler().makeLastFollower(*this);
  rootInteractionHandler().makeLastFollower(m_dragHandler);
  rootInteractionHandler().makeLastFollower(m_zoomHandler);
}

DewarpingView::~DewarpingView() = default;

void DewarpingView::initNewSpline(XSpline& spline,
                                  const QPointF& p1,
                                  const QPointF& p2,
                                  const DewarpingOptions* dewarpingOptions) {
  const QLineF line(p1, p2);
  spline.appendControlPoint(line.p1(), 0);
  if ((*dewarpingOptions).dewarpingMode() == AUTO) {
    spline.appendControlPoint(line.pointAt(1.0 / 4.0), 1);
    spline.appendControlPoint(line.pointAt(2.0 / 4.0), 1);
    spline.appendControlPoint(line.pointAt(3.0 / 4.0), 1);
  }
  spline.appendControlPoint(line.p2(), 0);
}

void DewarpingView::fitSpline(XSpline& spline, const std::vector<QPointF>& polyline) {
  using namespace spfit;

  SplineFitter fitter(&spline);
  const PolylineModelShape model_shape(polyline);

  ConstraintSet constraints(&spline);
  constraints.constrainSplinePoint(0.0, polyline.front());
  constraints.constrainSplinePoint(1.0, polyline.back());
  fitter.setConstraints(constraints);

  FittableSpline::SamplingParams sampling_params;
  sampling_params.maxDistBetweenSamples = 10;
  fitter.setSamplingParams(sampling_params);

  int iterations_remaining = 20;
  LinearForceBalancer balancer(0.8);
  balancer.setTargetRatio(0.1);
  balancer.setIterationsToTarget(iterations_remaining - 1);

  for (; iterations_remaining > 0; --iterations_remaining, balancer.nextIteration()) {
    fitter.addAttractionForces(model_shape);
    fitter.addInternalForce(spline.controlPointsAttractionForce());

    double internal_force_weight = balancer.calcInternalForceWeight(fitter.internalForce(), fitter.externalForce());
    const OptimizationResult res(fitter.optimize(internal_force_weight));
    if (dewarping::Curve::splineHasLoops(spline)) {
      fitter.undoLastStep();
      break;
    }

    if (res.improvementPercentage() < 0.5) {
      break;
    }
  }
}  // DewarpingView::fitSpline

void DewarpingView::depthPerceptionChanged(double val) {
  m_depthPerception.setValue(val);
  update();
}

void DewarpingView::onPaint(QPainter& painter, const InteractionState& interaction) {
  painter.setRenderHint(QPainter::Antialiasing);

  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(0xff, 0xff, 0xff, 150));  // Translucent white.
  painter.drawPolygon(virtMarginArea(0));           // Left margin.
  painter.drawPolygon(virtMarginArea(1));           // Right margin.
  painter.setWorldTransform(imageToVirtual() * painter.worldTransform());
  painter.setBrush(Qt::NoBrush);

  QPen grid_pen;
  grid_pen.setColor(Qt::blue);
  grid_pen.setCosmetic(true);
  grid_pen.setWidthF(1.2);

  painter.setPen(grid_pen);
  painter.setBrush(Qt::NoBrush);

  const int num_vert_grid_lines = 30;
  const int num_hor_grid_lines = 30;

  bool valid_model = m_distortionModel.isValid();

  if (valid_model) {
    try {
      std::vector<QVector<QPointF>> curves(num_hor_grid_lines);

      dewarping::CylindricalSurfaceDewarper dewarper(m_distortionModel.topCurve().polyline(),
                                                     m_distortionModel.bottomCurve().polyline(),
                                                     m_depthPerception.value());
      dewarping::CylindricalSurfaceDewarper::State state;

      for (int j = 0; j < num_vert_grid_lines; ++j) {
        const double x = j / (num_vert_grid_lines - 1.0);
        const dewarping::CylindricalSurfaceDewarper::Generatrix gtx(dewarper.mapGeneratrix(x, state));
        const QPointF gtx_p0(gtx.imgLine.pointAt(gtx.pln2img(0)));
        const QPointF gtx_p1(gtx.imgLine.pointAt(gtx.pln2img(1)));
        painter.drawLine(gtx_p0, gtx_p1);
        for (int i = 0; i < num_hor_grid_lines; ++i) {
          const double y = i / (num_hor_grid_lines - 1.0);
          curves[i].push_back(gtx.imgLine.pointAt(gtx.pln2img(y)));
        }
      }

      for (const QVector<QPointF>& curve : curves) {
        painter.drawPolyline(curve);
      }
    } catch (const std::runtime_error&) {
      // Still probably a bad model, even though DistortionModel::isValid() was true.
      valid_model = false;
    }
  }  // valid_model
  if (!valid_model) {
    // Just draw the frame.
    const dewarping::Curve& top_curve = m_distortionModel.topCurve();
    const dewarping::Curve& bottom_curve = m_distortionModel.bottomCurve();
    painter.drawLine(top_curve.polyline().front(), bottom_curve.polyline().front());
    painter.drawLine(top_curve.polyline().back(), bottom_curve.polyline().back());
    painter.drawPolyline(QVector<QPointF>::fromStdVector(top_curve.polyline()));
    painter.drawPolyline(QVector<QPointF>::fromStdVector(bottom_curve.polyline()));
  }

  paintXSpline(painter, interaction, m_topSpline);
  paintXSpline(painter, interaction, m_bottomSpline);
}  // DewarpingView::onPaint

void DewarpingView::paintXSpline(QPainter& painter,
                                 const InteractionState& interaction,
                                 const InteractiveXSpline& ispline) {
  const XSpline& spline = ispline.spline();

  painter.save();
  painter.setBrush(Qt::NoBrush);

#if 0  // No point in drawing the curve itself - we already draw the grid.
        painter.setWorldTransform(imageToVirtual() * virtualToWidget());

        QPen curve_pen(Qt::blue);
        curve_pen.setWidthF(1.5);
        curve_pen.setCosmetic(true);
        painter.setPen(curve_pen);

        const std::vector<QPointF> polyline(spline.toPolyline());
        painter.drawPolyline(&polyline[0], polyline.size());
#endif
  // Drawing cosmetic points in transformed coordinates seems unreliable,
  // so let's draw them in widget coordinates.
  painter.setWorldMatrixEnabled(false);

  QPen existing_point_pen(Qt::red);
  existing_point_pen.setWidthF(4.0);
  existing_point_pen.setCosmetic(true);
  painter.setPen(existing_point_pen);

  const int num_control_points = spline.numControlPoints();
  for (int i = 0; i < num_control_points; ++i) {
    painter.drawPoint(sourceToWidget(spline.controlPointPosition(i)));
  }

  QPointF pt;
  if (ispline.curveIsProximityLeader(interaction, &pt)) {
    QPen new_point_pen(existing_point_pen);
    new_point_pen.setColor(QColor(0x00ffff));
    painter.setPen(new_point_pen);
    painter.drawPoint(pt);
  }

  painter.restore();
}  // DewarpingView::paintXSpline

void DewarpingView::curveModified(int curve_idx) {
  if (curve_idx == 0) {
    m_distortionModel.setTopCurve(dewarping::Curve(m_topSpline.spline()));
  } else {
    m_distortionModel.setBottomCurve(dewarping::Curve(m_bottomSpline.spline()));
  }
  update();
}

void DewarpingView::dragFinished() {
  if ((m_dewarpingOptions.dewarpingMode() == AUTO) || (m_dewarpingOptions.dewarpingMode() == MARGINAL)) {
    m_dewarpingOptions.setDewarpingMode(MANUAL);
  }
  emit distortionModelChanged(m_distortionModel);
}

/** Source image coordinates to widget coordinates. */
QPointF DewarpingView::sourceToWidget(const QPointF& pt) const {
  return virtualToWidget().map(imageToVirtual().map(pt));
}

/** Widget coordinates to source image coordinates. */
QPointF DewarpingView::widgetToSource(const QPointF& pt) const {
  return virtualToImage().map(widgetToVirtual().map(pt));
}

QPolygonF DewarpingView::virtMarginArea(int margin_idx) const {
  const dewarping::Curve& top_curve = m_distortionModel.topCurve();
  const dewarping::Curve& bottom_curve = m_distortionModel.bottomCurve();

  QLineF vert_boundary;   // From top to bottom, that's important!
  if (margin_idx == 0) {  // Left margin.
    vert_boundary.setP1(top_curve.polyline().front());
    vert_boundary.setP2(bottom_curve.polyline().front());
  } else {  // Right margin.
    vert_boundary.setP1(top_curve.polyline().back());
    vert_boundary.setP2(bottom_curve.polyline().back());
  }

  vert_boundary = imageToVirtual().map(vert_boundary);

  QLineF normal;
  if (margin_idx == 0) {  // Left margin.
    normal = QLineF(vert_boundary.p2(), vert_boundary.p1()).normalVector();
  } else {  // Right margin.
    normal = vert_boundary.normalVector();
  }

  // Project every vertex in the m_virtDisplayArea polygon
  // to vert_line and to its normal, keeping track min and max values.
  double min = NumericTraits<double>::max();
  double max = NumericTraits<double>::min();
  double normal_max = max;
  const ToLineProjector vert_line_projector(vert_boundary);
  const ToLineProjector normal_projector(normal);
  for (const QPointF& pt : m_virtDisplayArea) {
    const double p1 = vert_line_projector.projectionScalar(pt);
    if (p1 < min) {
      min = p1;
    }
    if (p1 > max) {
      max = p1;
    }

    const double p2 = normal_projector.projectionScalar(pt);
    if (p2 > normal_max) {
      normal_max = p2;
    }
  }

  // Workaround clipping bugs in QPolygon::intersected().
  min -= 1.0;
  max += 1.0;
  normal_max += 1.0;

  QPolygonF poly;
  poly << vert_boundary.pointAt(min);
  poly << vert_boundary.pointAt(max);
  poly << vert_boundary.pointAt(max) + normal.pointAt(normal_max) - normal.p1();
  poly << vert_boundary.pointAt(min) + normal.pointAt(normal_max) - normal.p1();

  return m_virtDisplayArea.intersected(poly);
}  // DewarpingView::virtMarginArea
}  // namespace output