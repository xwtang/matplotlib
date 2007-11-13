/* A rewrite of _backend_agg using PyCXX to handle ref counting, etc..
 */

#include <iostream>
#include <fstream>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <png.h>
#include <time.h>
#include <algorithm>

#include "agg_conv_transform.h"
#include "agg_conv_curve.h"
#include "agg_scanline_storage_aa.h"
#include "agg_scanline_storage_bin.h"
#include "agg_renderer_primitives.h"
#include "agg_span_image_filter_gray.h"
#include "agg_span_interpolator_linear.h"
#include "agg_span_allocator.h"
#include "util/agg_color_conv_rgb8.h"

#include "ft2font.h"
#include "_image.h"
#include "_backend_agg.h"
#include "mplutils.h"

#include "swig_runtime.h"
#include "MPL_isnan.h"

#define PY_ARRAY_TYPES_PREFIX NumPy
#include "numpy/arrayobject.h"
#include "agg_py_transforms.h"

#ifndef M_PI
#define M_PI       3.14159265358979323846
#endif
#ifndef M_PI_4
#define M_PI_4     0.785398163397448309616
#endif
#ifndef M_PI_2
#define M_PI_2     1.57079632679489661923
#endif


/*
 Convert dashes from the Python representation as nested sequences to
 the C++ representation as a std::vector<std::pair<double, double> >
 (GCAgg::dash_t)
*/
void convert_dashes(const Py::Tuple& dashes, double dpi, GCAgg::dash_t& dashes_out, 
		    double& dashOffset_out) {
  if (dashes.length()!=2)
    throw Py::ValueError(Printf("Dash descriptor must be a length 2 tuple; found %d", dashes.length()).str());

  dashes_out.clear();
  dashOffset_out = 0.0;
  if (dashes[0].ptr() == Py_None)
    return;
  
  dashOffset_out = double(Py::Float(dashes[0])) * dpi/72.0;

  Py::SeqBase<Py::Object> dashSeq = dashes[1];
  
  size_t Ndash = dashSeq.length();
  if (Ndash % 2 != 0)
    throw Py::ValueError(Printf("Dash sequence must be an even length sequence; found %d", Ndash).str());

  dashes_out.clear();
  dashes_out.reserve(Ndash / 2);

  double val0, val1;
  for (size_t i = 0; i < Ndash; i += 2) {
    val0 = double(Py::Float(dashSeq[i])) * dpi/72.0;
    val1 = double(Py::Float(dashSeq[i+1])) * dpi/72.0;
    dashes_out.push_back(std::make_pair(val0, val1));
  }
}

Py::Object BufferRegion::to_string(const Py::Tuple &args) {
  // owned=true to prevent memory leak
  return Py::String(PyString_FromStringAndSize((const char*)aggbuf.data,aggbuf.height*aggbuf.stride), true);
}

template<class VertexSource> class conv_quantize
{
public:
  conv_quantize(VertexSource& source, bool quantize) :
    m_source(&source), m_quantize(quantize) {}
  
  void rewind(unsigned path_id) { 
    m_source->rewind(path_id); 
  }

  unsigned vertex(double* x, double* y) {
    unsigned cmd = m_source->vertex(x, y);
    if (m_quantize && agg::is_vertex(cmd)) {
      *x = round(*x) + 0.5;
      *y = round(*y) + 0.5;
    }
    return cmd;
  }

private:
  VertexSource* m_source;
  bool m_quantize;
};


GCAgg::GCAgg(const Py::Object &gc, double dpi) :
  dpi(dpi), isaa(true), linewidth(1.0), alpha(1.0),
  dashOffset(0.0)
{
  _VERBOSE("GCAgg::GCAgg");
  linewidth = points_to_pixels ( gc.getAttr("_linewidth") ) ;
  alpha = Py::Float( gc.getAttr("_alpha") );
  color = get_color(gc);
  _set_antialiased(gc);
  _set_linecap(gc);
  _set_joinstyle(gc);
  _set_dashes(gc);
  _set_clip_rectangle(gc);
  _set_clip_path(gc);
}

GCAgg::GCAgg(double dpi) :
  dpi(dpi), isaa(true), linewidth(1.0), alpha(1.0),
  dashOffset(0.0)
{

}

void
GCAgg::_set_antialiased(const Py::Object& gc) {
  _VERBOSE("GCAgg::antialiased");
  isaa = Py::Int( gc.getAttr( "_antialiased") );
}

agg::rgba
GCAgg::get_color(const Py::Object& gc) {
  _VERBOSE("GCAgg::get_color");
  Py::Tuple rgb = Py::Tuple( gc.getAttr("_rgb") );
  
  double alpha = Py::Float( gc.getAttr("_alpha") );
  
  double r = Py::Float(rgb[0]);
  double g = Py::Float(rgb[1]);
  double b = Py::Float(rgb[2]);
  return agg::rgba(r, g, b, alpha);
}

double
GCAgg::points_to_pixels( const Py::Object& points) {
  _VERBOSE("GCAgg::points_to_pixels");
  double p = Py::Float( points ) ;
  return p * dpi/72.0;
}

void
GCAgg::_set_linecap(const Py::Object& gc) {
  _VERBOSE("GCAgg::_set_linecap");
  
  std::string capstyle = Py::String( gc.getAttr( "_capstyle" ) );

  if (capstyle=="butt")
    cap = agg::butt_cap;
  else if (capstyle=="round")
    cap = agg::round_cap;
  else if(capstyle=="projecting")
    cap = agg::square_cap;
  else
    throw Py::ValueError(Printf("GC _capstyle attribute must be one of butt, round, projecting; found %s", capstyle.c_str()).str());
}

void
GCAgg::_set_joinstyle(const Py::Object& gc) {
  _VERBOSE("GCAgg::_set_joinstyle");
  
  std::string joinstyle = Py::String( gc.getAttr("_joinstyle") );
  
  if (joinstyle=="miter")
    join =  agg::miter_join;
  else if (joinstyle=="round")
    join = agg::round_join;
  else if(joinstyle=="bevel")
    join = agg::bevel_join;
  else
    throw Py::ValueError(Printf("GC _joinstyle attribute must be one of butt, round, projecting; found %s", joinstyle.c_str()).str());
}

void
GCAgg::_set_dashes(const Py::Object& gc) {
  //return the dashOffset, dashes sequence tuple.
  _VERBOSE("GCAgg::_set_dashes");
  
  Py::Object dash_obj( gc.getAttr( "_dashes" ) );
  if (dash_obj.ptr() == Py_None) {
    dashes.clear();
    return;
  }

  convert_dashes(dash_obj, dpi, dashes, dashOffset);
}

void
GCAgg::_set_clip_rectangle( const Py::Object& gc) {
  //set the clip rectangle from the gc
  
  _VERBOSE("GCAgg::_set_clip_rectangle");

  Py::Object o ( gc.getAttr( "_cliprect" ) );
  cliprect = o;
}

void
GCAgg::_set_clip_path( const Py::Object& gc) {
  //set the clip path from the gc
  
  _VERBOSE("GCAgg::_set_clip_path");
  
  Py::Object method_obj = gc.getAttr("get_clip_path");
  Py::Callable method(method_obj);
  Py::Tuple path_and_transform = method.apply(Py::Tuple());
  if (path_and_transform[0].ptr() != Py_None) {
    clippath = path_and_transform[0];
    clippath_trans = py_to_agg_transformation_matrix(path_and_transform[1]);
  }
}


const size_t
RendererAgg::PIXELS_PER_INCH(96);

RendererAgg::RendererAgg(unsigned int width, unsigned int height, double dpi,
			 int debug) :
  width(width),
  height(height),
  dpi(dpi),
  NUMBYTES(width*height*4),
  debug(debug)
{
  _VERBOSE("RendererAgg::RendererAgg");
  unsigned stride(width*4);
  
  
  pixBuffer	  = new agg::int8u[NUMBYTES];
  renderingBuffer = new agg::rendering_buffer;
  renderingBuffer->attach(pixBuffer, width, height, stride);
  
  alphaBuffer		   = new agg::int8u[NUMBYTES];
  alphaMaskRenderingBuffer = new agg::rendering_buffer;
  alphaMaskRenderingBuffer->attach(alphaBuffer, width, height, stride);
  alphaMask		   = new alpha_mask_type(*alphaMaskRenderingBuffer);
  //jdh
  pixfmtAlphaMask	   = new agg::pixfmt_gray8(*alphaMaskRenderingBuffer);
  rendererBaseAlphaMask	   = new renderer_base_alpha_mask_type(*pixfmtAlphaMask);
  rendererAlphaMask	   = new renderer_alpha_mask_type(*rendererBaseAlphaMask);
  scanlineAlphaMask	   = new agg::scanline_p8();
  
  
  slineP8  = new scanline_p8;
  slineBin = new scanline_bin;
  
  pixFmt       = new pixfmt(*renderingBuffer);
  rendererBase = new renderer_base(*pixFmt);
  rendererBase->clear(agg::rgba(1, 1, 1, 0));
  
  rendererAA	= new renderer_aa(*rendererBase);
  rendererBin	= new renderer_bin(*rendererBase);
  theRasterizer = new rasterizer();
  //theRasterizer->filling_rule(agg::fill_even_odd);
  //theRasterizer->filling_rule(agg::fill_non_zero);
  
};

template<class R>
void
RendererAgg::set_clipbox(const Py::Object& cliprect, R rasterizer) {
  //set the clip rectangle from the gc
  
  _VERBOSE("RendererAgg::set_clipbox");

  double l, b, r, t;
  if (py_convert_bbox(cliprect.ptr(), l, b, r, t)) {
    rasterizer->clip_box((int)l + 1, (int)b + 1, (int)r + 1, (int)t + 1);
  }

  _VERBOSE("RendererAgg::set_clipbox done");
}

std::pair<bool, agg::rgba>
RendererAgg::_get_rgba_face(const Py::Object& rgbFace, double alpha) {
  _VERBOSE("RendererAgg::_get_rgba_face");
  std::pair<bool, agg::rgba> face;
  
  if (rgbFace.ptr() == Py_None) {
    face.first = false;
  }
  else {
    face.first = true;
    Py::Tuple rgb = Py::Tuple(rgbFace);
    face.second = rgb_to_color(rgb, alpha);
  }
  return face;
}

SnapData
SafeSnap::snap (const float& x, const float& y) {
  xsnap = (int)x + 0.5;
  ysnap = (int)y + 0.5;
  
  if ( first || ( (xsnap!=lastxsnap) || (ysnap!=lastysnap) ) ) {
    lastxsnap = xsnap;
    lastysnap = ysnap;
    lastx = x;
    lasty = y;
    first = false;
    return SnapData(true, xsnap, ysnap);
  }

  // ok both are equal and we need to do an offset
  if ( (x==lastx) && (y==lasty) ) {
    // no choice but to return equal coords; set newpoint = false
    lastxsnap = xsnap;
    lastysnap = ysnap;
    lastx = x;
    lasty = y;
    return SnapData(false, xsnap, ysnap);    
  }

  // ok the real points are not identical but the rounded ones, so do
  // a one pixel offset
  if (x>lastx) xsnap += 1.;
  else if (x<lastx) xsnap -= 1.;

  if (y>lasty) ysnap += 1.;
  else if (y<lasty) ysnap -= 1.;

  lastxsnap = xsnap;
  lastysnap = ysnap;
  lastx = x;
  lasty = y;
  return SnapData(true, xsnap, ysnap);    
}  

template<class Path>
bool should_snap(Path& path, const agg::trans_affine& trans) {
  // If this is a straight horizontal or vertical line, quantize to nearest 
  // pixels
  double x0, y0, x1, y1;
  unsigned code;
  code = path.vertex(&x0, &y0);
  trans.transform(&x0, &y0);

  while ((code = path.vertex(&x1, &y1)) != agg::path_cmd_stop) {

    switch (code) {
    case agg::path_cmd_curve3:
    case agg::path_cmd_curve4:
      path.rewind(0);
      return false;
    }

    trans.transform(&x1, &y1);
    if (!(fabs(x0 - x1) < 1e-4 || fabs(y0 - y1) < 1e-4)) {
      path.rewind(0);
      return false;
    }
    x0 = x1;
    y0 = y1;
  }

  path.rewind(0);
  return true;
}

Py::Object
RendererAgg::copy_from_bbox(const Py::Tuple& args) {
  //copy region in bbox to buffer and return swig/agg buffer object
  args.verify_length(1);

  Py::Object box_obj = args[0];
  double l, b, r, t;
  if (!py_convert_bbox(box_obj.ptr(), l, b, r, t)) 
    throw Py::TypeError("Invalid bbox provided to copy_from_bbox");
  
  agg::rect rect((int)l, (int)b, (int)r, (int)t);

  int boxwidth = rect.x2-rect.x1;
  int boxheight = rect.y2-rect.y1;
  int boxstride = boxwidth*4;
  agg::buffer buf(boxwidth, boxheight, boxstride, false);
  if (buf.data ==NULL) {
    throw Py::MemoryError("RendererAgg::copy_from_bbox could not allocate memory for buffer");
  }
  
  agg::rendering_buffer rbuf;
  rbuf.attach(buf.data, boxwidth, boxheight, boxstride);
  
  pixfmt pf(rbuf);
  renderer_base rb(pf);
  //rb.clear(agg::rgba(1, 0, 0)); //todo remove me
  rb.copy_from(*renderingBuffer, &rect, -rect.x1, -rect.y1);
  BufferRegion* reg = new BufferRegion(buf, rect, true);
  return Py::asObject(reg);
}

Py::Object
RendererAgg::restore_region(const Py::Tuple& args) {
  //copy BufferRegion to buffer
  args.verify_length(1);
  BufferRegion* region  = static_cast<BufferRegion*>(args[0].ptr());
  
  if (region->aggbuf.data==NULL)
    return Py::Object();
  //throw Py::ValueError("Cannot restore_region from NULL data");
  
  
  agg::rendering_buffer rbuf;
  rbuf.attach(region->aggbuf.data,
	      region->aggbuf.width,
	      region->aggbuf.height,
	      region->aggbuf.stride);
  
  rendererBase->copy_from(rbuf, 0, region->rect.x1, region->rect.y1);
  
  return Py::Object();
}

bool RendererAgg::render_clippath(const Py::Object& clippath, const agg::trans_affine& clippath_trans) {
  typedef agg::conv_transform<PathIterator> transformed_path_t;
  typedef agg::conv_curve<transformed_path_t> curve_t;

  bool has_clippath = (clippath.ptr() != Py_None);

  if (has_clippath && 
      (clippath.ptr() != lastclippath.ptr() || 
       clippath_trans != lastclippath_transform)) {
    agg::trans_affine trans(clippath_trans);
    trans *= agg::trans_affine_scaling(1.0, -1.0);
    trans *= agg::trans_affine_translation(0.0, (double)height);

    PathIterator clippath_iter(clippath);
    rendererBaseAlphaMask->clear(agg::gray8(0, 0));
    transformed_path_t transformed_clippath(clippath_iter, trans);
    agg::conv_curve<transformed_path_t> curved_clippath(transformed_clippath);
    theRasterizer->add_path(curved_clippath);
    rendererAlphaMask->color(agg::gray8(255, 255));
    agg::render_scanlines(*theRasterizer, *scanlineAlphaMask, *rendererAlphaMask);
    lastclippath = clippath;
    lastclippath_transform = clippath_trans;
  }

  return has_clippath;
}

Py::Object
RendererAgg::draw_markers(const Py::Tuple& args) {
  typedef agg::conv_transform<PathIterator>		     transformed_path_t;
  typedef conv_quantize<transformed_path_t>		     quantize_t;
  typedef agg::conv_curve<transformed_path_t>	             curve_t;
  typedef agg::conv_stroke<curve_t>			     stroke_t;
  typedef agg::pixfmt_amask_adaptor<pixfmt, alpha_mask_type> pixfmt_amask_type;
  typedef agg::renderer_base<pixfmt_amask_type>		     amask_ren_type;
  typedef agg::renderer_scanline_aa_solid<amask_ren_type>    amask_aa_renderer_type;
  typedef agg::renderer_scanline_bin_solid<amask_ren_type>   amask_bin_renderer_type;

  rendererBase->reset_clipping(true);
  theRasterizer->reset_clipping();
  
  args.verify_length(5, 6);
  
  Py::Object	    gc_obj	    = args[0];
  Py::Object	    marker_path_obj = args[1];
  agg::trans_affine marker_trans    = py_to_agg_transformation_matrix(args[2]);
  Py::Object	    path_obj	    = args[3];
  agg::trans_affine trans	    = py_to_agg_transformation_matrix(args[4]);
  Py::Object        face_obj;
  if (args.size() == 6)
    face_obj = args[5];

  // Deal with the difference in y-axis direction
  marker_trans *= agg::trans_affine_scaling(1.0, -1.0);
  trans *= agg::trans_affine_scaling(1.0, -1.0);
  trans *= agg::trans_affine_translation(0.0, (double)height);
  
  PathIterator marker_path(marker_path_obj);
  transformed_path_t marker_path_transformed(marker_path, marker_trans);
  curve_t marker_path_curve(marker_path_transformed);

  PathIterator path(path_obj);
  bool snap = should_snap(path, trans);
  transformed_path_t path_transformed(path, trans);
  GCAgg gc = GCAgg(gc_obj, dpi);
  quantize_t path_quantized(path_transformed, snap);
  path_quantized.rewind(0);

  facepair_t face = _get_rgba_face(face_obj, gc.alpha);
  
  //maxim's suggestions for cached scanlines
  agg::scanline_storage_aa8 scanlines;
  theRasterizer->reset();
  
  agg::int8u* fillCache = NULL;
  agg::int8u* strokeCache = NULL;

  try {
    unsigned fillSize = 0;
    if (face.first) {
      theRasterizer->add_path(marker_path_curve);
      agg::render_scanlines(*theRasterizer, *slineP8, scanlines);
      fillSize = scanlines.byte_size();
      fillCache = new agg::int8u[fillSize]; // or any container
      scanlines.serialize(fillCache);
    }
  
    stroke_t stroke(marker_path_curve);
    stroke.width(gc.linewidth);
    stroke.line_cap(gc.cap);
    stroke.line_join(gc.join);
    theRasterizer->reset();
    theRasterizer->add_path(stroke);
    agg::render_scanlines(*theRasterizer, *slineP8, scanlines);
    unsigned strokeSize = scanlines.byte_size();
    strokeCache = new agg::int8u[strokeSize]; // or any container
    scanlines.serialize(strokeCache);
    
    theRasterizer->reset_clipping();
    rendererBase->reset_clipping(true);
    set_clipbox(gc.cliprect, rendererBase);
    bool has_clippath = render_clippath(gc.clippath, gc.clippath_trans);
    
    double x, y;

    agg::serialized_scanlines_adaptor_aa8 sa;
    agg::serialized_scanlines_adaptor_aa8::embedded_scanline sl;

    while (path_quantized.vertex(&x, &y) != agg::path_cmd_stop) {
      //render the fill
      if (face.first) {
	if (has_clippath) {
	  pixfmt_amask_type pfa(*pixFmt, *alphaMask);
	  amask_ren_type r(pfa);
	  amask_aa_renderer_type ren(r);
	  sa.init(fillCache, fillSize, x, y);
	  ren.color(face.second);
	  agg::render_scanlines(sa, sl, ren);
	} else {
	  sa.init(fillCache, fillSize, x, y);
	  rendererAA->color(face.second);
	  agg::render_scanlines(sa, sl, *rendererAA);
	}
      }
      
      //render the stroke
      if (has_clippath) {
	pixfmt_amask_type pfa(*pixFmt, *alphaMask);
	amask_ren_type r(pfa);
	amask_aa_renderer_type ren(r);
	sa.init(strokeCache, strokeSize, x, y);
	ren.color(gc.color);
	agg::render_scanlines(sa, sl, ren);
      } else {
	sa.init(strokeCache, strokeSize, x, y);
	rendererAA->color(gc.color);
	agg::render_scanlines(sa, sl, *rendererAA);
      }
    }
  } catch(...) {
    delete[] fillCache;
    delete[] strokeCache;
    throw;
  }
  
  delete [] fillCache;
  delete [] strokeCache;

  return Py::Object();
  
}

/**
 * This is a custom span generator that converts spans in the 
 * 8-bit inverted greyscale font buffer to rgba that agg can use.
 */
template<
  class ColorT,
  class ChildGenerator>
class font_to_rgba :
  public agg::span_generator<ColorT, 
			     agg::span_allocator<ColorT> >
{
public:
  typedef ChildGenerator child_type;
  typedef ColorT color_type;
  typedef agg::span_allocator<color_type> allocator_type;
  typedef agg::span_generator<
    ColorT, 
    agg::span_allocator<ColorT> > base_type;

private:
  child_type* _gen;
  allocator_type _alloc;
  color_type _color;
  
public:
  font_to_rgba(child_type* gen, color_type color) : 
    base_type(_alloc),
    _gen(gen),
    _color(color) {
  }

  color_type* generate(int x, int y, unsigned len)
  {
    color_type* dst = base_type::allocator().span();

    typename child_type::color_type* src = _gen->generate(x, y, len);

    do {
      *dst = _color;
      dst->a = src->v;
      ++src;
      ++dst;
    } while (--len);

    return base_type::allocator().span();
  }

  void prepare(unsigned max_span_len) 
  {
    _alloc.allocate(max_span_len);
    _gen->prepare(max_span_len);
  }

};

// MGDTODO: Support clip paths
Py::Object
RendererAgg::draw_text_image(const Py::Tuple& args) {
  _VERBOSE("RendererAgg::draw_text");

  typedef agg::span_interpolator_linear<> interpolator_type;
  typedef agg::span_image_filter_gray<agg::gray8, interpolator_type> 
    image_span_gen_type;
  typedef font_to_rgba<pixfmt::color_type, image_span_gen_type> 
    span_gen_type;
  typedef agg::renderer_scanline_aa<renderer_base, span_gen_type> 
    renderer_type;
  
  args.verify_length(5);
  
  FT2Image *image = static_cast<FT2Image*>(args[0].ptr());
  if (!image->get_buffer())
    return Py::Object();
  
  int x(0),y(0);
  try {
    x = Py::Int( args[1] );
    y = Py::Int( args[2] );
  }
  catch (Py::TypeError) {
    //x,y out of range; todo issue warning?
    return Py::Object();
  }
  
  double angle = Py::Float( args[3] );

  GCAgg gc = GCAgg(args[4], dpi);
  
  theRasterizer->reset_clipping();
  rendererBase->reset_clipping(true);
  set_clipbox(gc.cliprect, theRasterizer);

  const unsigned char* const buffer = image->get_buffer();
  agg::rendering_buffer srcbuf
    ((agg::int8u*)buffer, image->get_width(), 
     image->get_height(), image->get_width());
  agg::pixfmt_gray8 pixf_img(srcbuf);
  
  agg::trans_affine mtx;
  mtx *= agg::trans_affine_translation(0, -(int)image->get_height());
  mtx *= agg::trans_affine_rotation(-angle * agg::pi / 180.0);
  mtx *= agg::trans_affine_translation(x, y);

  agg::path_storage rect;
  rect.move_to(0, 0);
  rect.line_to(image->get_width(), 0);
  rect.line_to(image->get_width(), image->get_height());
  rect.line_to(0, image->get_height());
  rect.line_to(0, 0);
  agg::conv_transform<agg::path_storage> rect2(rect, mtx);

  agg::trans_affine inv_mtx(mtx);
  inv_mtx.invert();

  agg::image_filter_lut filter;
  filter.calculate(agg::image_filter_spline36());
  interpolator_type interpolator(inv_mtx);
  agg::span_allocator<agg::gray8> gray_span_allocator;
  image_span_gen_type image_span_generator(gray_span_allocator, 
					   srcbuf, 0, interpolator, filter);
  span_gen_type output_span_generator(&image_span_generator, gc.color);
  renderer_type ri(*rendererBase, output_span_generator);
  //agg::rasterizer_scanline_aa<> rasterizer;
  //agg::scanline_p8 scanline;
  //rasterizer.add_path(rect2);
  //agg::render_scanlines(rasterizer, scanline, ri);

  
  theRasterizer->add_path(rect2);
  agg::render_scanlines(*theRasterizer, *slineP8, ri);

  
  return Py::Object();
}


// MGDTODO: Support clip paths
Py::Object
RendererAgg::draw_image(const Py::Tuple& args) {
  _VERBOSE("RendererAgg::draw_image");

  args.verify_length(4, 6);
  
  float x = Py::Float(args[0]);
  float y = Py::Float(args[1]);
  Image *image = static_cast<Image*>(args[2].ptr());
  Py::Object box_obj = args[3];
  Py::Object clippath;
  agg::trans_affine clippath_trans;
  if (args.size() == 6) {
    clippath = args[4];
    clippath_trans = py_to_agg_transformation_matrix(args[5], false);
  }
 
  theRasterizer->reset_clipping();
  rendererBase->reset_clipping(true);
  set_clipbox(box_obj, rendererBase);

  Py::Tuple empty;
  pixfmt pixf(*(image->rbufOut));
  image->flipud_out(empty);

  rendererBase->blend_from(pixf, 0, (int)x, (int)(height-(y+image->rowsOut)));

  image->flipud_out(empty);
  
  return Py::Object();
}

template<class PathIteratorType>
void RendererAgg::_draw_path(PathIteratorType& path, agg::trans_affine trans, 
			     bool has_clippath, const facepair_t& face, 
			     const GCAgg& gc, bool check_snap) {
  typedef agg::conv_transform<PathIteratorType>		     transformed_path_t;
  typedef conv_quantize<transformed_path_t>		     quantize_t;
  typedef agg::conv_curve<quantize_t>			     curve_t;
  typedef agg::conv_stroke<curve_t>			     stroke_t;
  typedef agg::conv_dash<curve_t>			     dash_t;
  typedef agg::conv_stroke<dash_t>			     stroke_dash_t;
  typedef agg::pixfmt_amask_adaptor<pixfmt, alpha_mask_type> pixfmt_amask_type;
  typedef agg::renderer_base<pixfmt_amask_type>		     amask_ren_type;
  typedef agg::renderer_scanline_aa_solid<amask_ren_type>    amask_aa_renderer_type;
  typedef agg::renderer_scanline_bin_solid<amask_ren_type>   amask_bin_renderer_type;

  trans *= agg::trans_affine_scaling(1.0, -1.0);
  trans *= agg::trans_affine_translation(0.0, (double)height);

  // Build the transform stack
  bool snap = false;
  if (check_snap)
    snap = should_snap(path, trans);
  transformed_path_t tpath(path, trans);
  quantize_t quantized(tpath, snap);
  // Benchmarking shows that there is no noticable slowdown to always
  // treating paths as having curved segments.  Doing so greatly 
  // simplifies the code
  curve_t curve(quantized);

  // Render face
  if (face.first) {
    theRasterizer->add_path(curve);

    if (gc.isaa) {
      if (has_clippath) {
	pixfmt_amask_type pfa(*pixFmt, *alphaMask);
	amask_ren_type r(pfa);
	amask_aa_renderer_type ren(r);
	ren.color(face.second);
	agg::render_scanlines(*theRasterizer, *slineP8, ren);
      } else {
	rendererAA->color(face.second);
	agg::render_scanlines(*theRasterizer, *slineP8, *rendererAA);
      }
    } else {
      if (has_clippath) {
	pixfmt_amask_type pfa(*pixFmt, *alphaMask);
	amask_ren_type r(pfa);
	amask_bin_renderer_type ren(r);
	ren.color(face.second);
	agg::render_scanlines(*theRasterizer, *slineP8, ren);
      } else {
	rendererBin->color(face.second);
	agg::render_scanlines(*theRasterizer, *slineP8, *rendererBin);
      }
    }
  }

  // Render stroke
  if (gc.linewidth != 0.0) {
    double linewidth = gc.linewidth;
    if (snap)
      linewidth = round(linewidth);

    if (gc.dashes.size() == 0) {
      stroke_t stroke(curve);
      stroke.width(linewidth);
      stroke.line_cap(gc.cap);
      stroke.line_join(gc.join);
      theRasterizer->add_path(stroke);
    } else {
      dash_t dash(curve);
      for (GCAgg::dash_t::const_iterator i = gc.dashes.begin();
	   i != gc.dashes.end(); ++i) {
	double val0 = i->first;
	double val1 = i->second;
	if (snap) {
	  val0 = (int)val0 + 0.5;
	  val1 = (int)val1 + 0.5;
	}
	dash.add_dash(val0, val1);
      } 
      stroke_dash_t stroke(dash);
      stroke.line_cap(gc.cap);
      stroke.line_join(gc.join);
      stroke.width(linewidth);
      theRasterizer->add_path(stroke);
    }
    
    if (gc.isaa && !(snap)) {
      if (has_clippath) {
	pixfmt_amask_type pfa(*pixFmt, *alphaMask);
	amask_ren_type r(pfa);
	amask_aa_renderer_type ren(r);
	ren.color(gc.color);
	agg::render_scanlines(*theRasterizer, *slineP8, ren);
      } else {
	rendererAA->color(gc.color);
	agg::render_scanlines(*theRasterizer, *slineP8, *rendererAA);
      }
    } else {
      if (has_clippath) {
	pixfmt_amask_type pfa(*pixFmt, *alphaMask);
	amask_ren_type r(pfa);
	amask_bin_renderer_type ren(r);
	ren.color(gc.color);
	agg::render_scanlines(*theRasterizer, *slineP8, ren);
      } else {
	rendererBin->color(gc.color);
	agg::render_scanlines(*theRasterizer, *slineBin, *rendererBin);
      }
    }
  }
}	     			    

Py::Object
RendererAgg::draw_path(const Py::Tuple& args) {
  _VERBOSE("RendererAgg::draw_path");
  args.verify_length(3, 4);

  Py::Object gc_obj = args[0];
  Py::Object path_obj = args[1];
  agg::trans_affine trans = py_to_agg_transformation_matrix(args[2]);
  Py::Object face_obj;
  if (args.size() == 4)
    face_obj = args[3];

  PathIterator path(path_obj);
  GCAgg gc = GCAgg(gc_obj, dpi);
  facepair_t face = _get_rgba_face(face_obj, gc.alpha);
  
  theRasterizer->reset_clipping();
  rendererBase->reset_clipping(true);
  set_clipbox(gc.cliprect, theRasterizer);
  bool has_clippath = render_clippath(gc.clippath, gc.clippath_trans);

  _draw_path(path, trans, has_clippath, face, gc, true);
  
  return Py::Object();
}

template<class PathGenerator>
Py::Object
RendererAgg::_draw_path_collection_generic
  (const agg::trans_affine&	  master_transform,
   const Py::Object&		  cliprect,
   const Py::Object&		  clippath,
   const agg::trans_affine&       clippath_trans,
   const PathGenerator&		  path_generator,
   const Py::SeqBase<Py::Object>& transforms_obj,
   const Py::Object&              offsets_obj,
   const agg::trans_affine&       offset_trans,
   const Py::Object&              facecolors_obj,
   const Py::Object&              edgecolors_obj,
   const Py::SeqBase<Py::Float>&  linewidths,
   const Py::SeqBase<Py::Object>& linestyles_obj,
   const Py::SeqBase<Py::Int>&    antialiaseds,
   bool                           check_snap) {
  GCAgg gc(dpi);

  PyArrayObject* offsets    = NULL;
  PyArrayObject* facecolors = NULL;
  PyArrayObject* edgecolors = NULL;

  try {
    offsets = (PyArrayObject*)PyArray_FromObject(offsets_obj.ptr(), PyArray_DOUBLE, 0, 2);
    if (!offsets || 
	(PyArray_NDIM(offsets) == 2 && PyArray_DIM(offsets, 1) != 2) || 
	(PyArray_NDIM(offsets) == 1 && PyArray_DIM(offsets, 0) != 0)) {
      throw Py::ValueError("Offsets array must be Nx2");
    }

    PyArrayObject* facecolors = (PyArrayObject*)PyArray_FromObject
      (facecolors_obj.ptr(), PyArray_DOUBLE, 1, 2);
    if (!facecolors || 
	(PyArray_NDIM(facecolors) == 1 && PyArray_DIM(facecolors, 0) != 0) || 
	(PyArray_NDIM(facecolors) == 2 && PyArray_DIM(facecolors, 1) != 4))
      throw Py::ValueError("Facecolors must be a Nx4 numpy array or empty");

    PyArrayObject* edgecolors = (PyArrayObject*)PyArray_FromObject
      (edgecolors_obj.ptr(), PyArray_DOUBLE, 1, 2);
    if (!edgecolors || 
	(PyArray_NDIM(edgecolors) == 1 && PyArray_DIM(edgecolors, 0) != 0) || 
	(PyArray_NDIM(edgecolors) == 2 && PyArray_DIM(edgecolors, 1) != 4))
      throw Py::ValueError("Edgecolors must be a Nx4 numpy array");
    
    size_t Npaths      = path_generator.num_paths();
    size_t Noffsets    = offsets->dimensions[0];
    size_t N	       = std::max(Npaths, Noffsets);
    size_t Ntransforms = std::min(transforms_obj.length(), N);
    size_t Nfacecolors = facecolors->dimensions[0];
    size_t Nedgecolors = edgecolors->dimensions[0];
    size_t Nlinewidths = linewidths.length();
    size_t Nlinestyles = std::min(linestyles_obj.length(), N);
    size_t Naa	       = antialiaseds.length();

    if ((Nfacecolors == 0 && Nedgecolors == 0) || Npaths == 0)
      return Py::Object();
    
    size_t i = 0;
    
    // Convert all of the transforms up front
    typedef std::vector<agg::trans_affine> transforms_t;
    transforms_t transforms;
    transforms.reserve(Ntransforms);
    for (i = 0; i < Ntransforms; ++i) {
      agg::trans_affine trans = py_to_agg_transformation_matrix
	(transforms_obj[i], false);
      trans *= master_transform;
      transforms.push_back(trans);
    }
    
    // Convert all the dashes up front
    typedef std::vector<std::pair<double, GCAgg::dash_t> > dashes_t;
    dashes_t dashes;
    dashes.resize(Nlinestyles);
    i = 0;
    for (dashes_t::iterator d = dashes.begin(); 
	 d != dashes.end(); ++d, ++i) {
      convert_dashes(Py::Tuple(linestyles_obj[i]), dpi, d->second, d->first);
    }
    
    // Handle any clipping globally
    theRasterizer->reset_clipping();
    rendererBase->reset_clipping(true);
    set_clipbox(cliprect, theRasterizer);
    bool has_clippath = render_clippath(clippath, clippath_trans);
    
    // Set some defaults, assuming no face or edge
    gc.linewidth = 0.0;
    facepair_t face;
    face.first = Nfacecolors != 0;
    agg::trans_affine trans;

    for (i = 0; i < N; ++i) {
      typename PathGenerator::path_iterator path = path_generator(i);
      if (Ntransforms) {
	trans = transforms[i % Ntransforms];
      } else {
	trans = master_transform;
      }

      if (Noffsets) {
	double xo = *(double*)PyArray_GETPTR2(offsets, i % Noffsets, 0);
	double yo = *(double*)PyArray_GETPTR2(offsets, i % Noffsets, 1);
	offset_trans.transform(&xo, &yo);
	trans *= agg::trans_affine_translation(xo, yo);
      }
      
      if (Nfacecolors) {
	size_t fi = i % Nfacecolors;
	face.second = agg::rgba(*(double*)PyArray_GETPTR2(facecolors, fi, 0),
				*(double*)PyArray_GETPTR2(facecolors, fi, 1),
				*(double*)PyArray_GETPTR2(facecolors, fi, 2),
				*(double*)PyArray_GETPTR2(facecolors, fi, 3));
      }
      
      if (Nedgecolors) {
	size_t ei = i % Nedgecolors;
	gc.color = agg::rgba(*(double*)PyArray_GETPTR2(edgecolors, ei, 0),
			     *(double*)PyArray_GETPTR2(edgecolors, ei, 1),
			     *(double*)PyArray_GETPTR2(edgecolors, ei, 2),
			     *(double*)PyArray_GETPTR2(edgecolors, ei, 3));
	if (Nlinewidths) {
	  gc.linewidth = double(Py::Float(linewidths[i % Nlinewidths])) * dpi/72.0;
	} else {
	  gc.linewidth = 1.0;
	}
	if (Nlinestyles) {
	  gc.dashes = dashes[i % Nlinestyles].second;
	  gc.dashOffset = dashes[i % Nlinestyles].first;
	}
      }
      
      gc.isaa = bool(Py::Int(antialiaseds[i % Naa]));

      _draw_path(path, trans, has_clippath, face, gc, check_snap);
    }
  } catch (...) {
    Py_XDECREF(offsets);
    Py_XDECREF(facecolors);
    Py_XDECREF(edgecolors);
    throw;
  }

  Py_XDECREF(offsets);
  return Py::Object();
}


class PathListGenerator {
  const Py::SeqBase<Py::Object>& m_paths;

public:
  typedef PathIterator path_iterator;

  inline PathListGenerator(const Py::SeqBase<Py::Object>& paths) :
    m_paths(paths) {

  }
  
  inline size_t num_paths() const {
    return m_paths.size();
  }

  inline path_iterator operator()(size_t i) const {
    return PathIterator(m_paths[i]);
  }
};

Py::Object
RendererAgg::draw_path_collection(const Py::Tuple& args) {
  _VERBOSE("RendererAgg::draw_path_collection");
  args.verify_length(13);
  
  //segments, trans, clipbox, colors, linewidths, antialiaseds
  agg::trans_affine	  master_transform = py_to_agg_transformation_matrix(args[0]);
  Py::Object		  cliprect	   = args[1];
  Py::Object		  clippath	   = args[2];
  agg::trans_affine       clippath_trans   = py_to_agg_transformation_matrix(args[3], false);
  Py::SeqBase<Py::Object> paths		   = args[4];
  Py::SeqBase<Py::Object> transforms_obj   = args[5];
  Py::Object              offsets_obj      = args[6];
  agg::trans_affine       offset_trans     = py_to_agg_transformation_matrix(args[7]);
  Py::Object              facecolors_obj   = args[8];
  Py::Object              edgecolors_obj   = args[9];
  Py::SeqBase<Py::Float>  linewidths	   = args[10];
  Py::SeqBase<Py::Object> linestyles_obj   = args[11];
  Py::SeqBase<Py::Int>    antialiaseds	   = args[12];

  PathListGenerator path_generator(paths);

  _draw_path_collection_generic
    (master_transform,
     cliprect,
     clippath,
     clippath_trans,
     path_generator,
     transforms_obj,
     offsets_obj,
     offset_trans,
     facecolors_obj,
     edgecolors_obj,
     linewidths,
     linestyles_obj,
     antialiaseds,
     true);

  return Py::Object();
}

class QuadMeshGenerator {
  size_t m_meshWidth;
  size_t m_meshHeight;
  PyArrayObject* m_coordinates;

  class QuadMeshPathIterator {
    size_t m_iterator;
    size_t m_m, m_n;
    PyArrayObject* m_coordinates;
  public:
    QuadMeshPathIterator(size_t m, size_t n, PyArrayObject* coordinates) :
      m_iterator(0), m_m(m), m_n(n), m_coordinates(coordinates) {
    }
    
    static const size_t offsets[5][2];

    inline unsigned vertex(unsigned idx, double* x, double* y) {
      size_t m = m_m + offsets[idx][0];
      size_t n = m_n + offsets[idx][1];
      *x = *(double*)PyArray_GETPTR3(m_coordinates, m, n, 0);
      *y = *(double*)PyArray_GETPTR3(m_coordinates, m, n, 1);
      return (idx == 0) ? agg::path_cmd_move_to : agg::path_cmd_line_to;
    }

    inline unsigned vertex(double* x, double* y) {
      if (m_iterator >= total_vertices()) return agg::path_cmd_stop;
      return vertex(m_iterator++, x, y);
    }

    inline void rewind(unsigned path_id) {
      m_iterator = path_id;
    }
    
    inline unsigned total_vertices() {
      return 5;
    }

    inline bool has_curves() {
      return false;
    }
  };

public:
  typedef QuadMeshPathIterator path_iterator;

  inline QuadMeshGenerator(size_t meshWidth, size_t meshHeight, const Py::Object& coordinates) :
    m_meshWidth(meshWidth), m_meshHeight(meshHeight), m_coordinates(NULL) {
    PyArrayObject* coordinates_array = (PyArrayObject*)PyArray_FromObject(coordinates.ptr(), PyArray_DOUBLE, 1, 3);
    if (!coordinates_array) {
      throw Py::ValueError("Invalid coordinates array.");
    }
    
    PyArray_Dims shape;
    npy_intp dims[] = { meshHeight + 1, meshWidth + 1, 2 };
    shape.ptr = dims;
    shape.len = 3;
    m_coordinates = (PyArrayObject*)PyArray_Newshape(coordinates_array, &shape, PyArray_CORDER);
  }
  
  inline ~QuadMeshGenerator() {
    Py_XDECREF(m_coordinates);
  }

  inline size_t num_paths() const {
    return m_meshWidth * m_meshHeight;
  }

  inline path_iterator operator()(size_t i) const {
    return QuadMeshPathIterator(i % m_meshHeight, i / m_meshHeight, m_coordinates);
  }
};

const size_t QuadMeshGenerator::QuadMeshPathIterator::offsets[5][2] = {
      { 0, 0 },
      { 0, 1 },
      { 1, 1 },
      { 1, 0 },
      { 0, 0 } };

Py::Object
RendererAgg::draw_quad_mesh(const Py::Tuple& args) {
  _VERBOSE("RendererAgg::draw_quad_mesh");
  args.verify_length(12);
  
  //segments, trans, clipbox, colors, linewidths, antialiaseds
  agg::trans_affine	  master_transform = py_to_agg_transformation_matrix(args[0]);
  Py::Object		  cliprect	   = args[1];
  Py::Object		  clippath	   = args[2];
  agg::trans_affine       clippath_trans   = py_to_agg_transformation_matrix(args[3], false);
  size_t                  mesh_width       = Py::Int(args[4]);
  size_t                  mesh_height      = Py::Int(args[5]);
  Py::Object              coordinates	   = args[6];
  Py::Object              offsets_obj      = args[7];
  agg::trans_affine       offset_trans     = py_to_agg_transformation_matrix(args[8]);
  Py::Object              facecolors_obj   = args[9];
  bool                    antialiased	   = (bool)Py::Int(args[10]);
  bool                    showedges        = (bool)Py::Int(args[11]);
  
  QuadMeshGenerator path_generator(mesh_width, mesh_height, coordinates);

  Py::SeqBase<Py::Object> transforms_obj;
  Py::Object edgecolors_obj;
  Py::Tuple linewidths(1);
  linewidths[0] = Py::Float(1.0);
  Py::SeqBase<Py::Object> linestyles_obj;
  Py::Tuple antialiaseds(1);
  antialiaseds[0] = Py::Int(antialiased ? 1 : 0);

  if (showedges) {
    int dims[] = { 1, 4, 0 };
    double data[] = { 0, 0, 0, 1 };
    edgecolors_obj = PyArray_SimpleNewFromData(2, dims, PyArray_DOUBLE, (char*)data);
  } else {
    if (antialiased) {
      edgecolors_obj = facecolors_obj;
    } else {
      int dims[] = { 0, 0 };
      edgecolors_obj = PyArray_SimpleNew(1, dims, PyArray_DOUBLE);
    }
  }

  _draw_path_collection_generic
    (master_transform,
     cliprect,
     clippath,
     clippath_trans,
     path_generator,
     transforms_obj,
     offsets_obj,
     offset_trans,
     facecolors_obj,
     edgecolors_obj,
     linewidths,
     linestyles_obj,
     antialiaseds,
     false);

  return Py::Object();
}

Py::Object
RendererAgg::write_rgba(const Py::Tuple& args) {
  _VERBOSE("RendererAgg::write_rgba");
  
  args.verify_length(1);
  std::string fname = Py::String(args[0]);
  
  std::ofstream of2( fname.c_str(), std::ios::binary|std::ios::out);
  for (size_t i=0; i<NUMBYTES; i++) {
    of2.write((char*)&(pixBuffer[i]), sizeof(char));
  }
  return Py::Object();
}

static void write_png_data(png_structp png_ptr, png_bytep data, png_size_t length) {
  PyObject* py_file_obj = (PyObject*)png_get_io_ptr(png_ptr);
  PyObject* write_method = PyObject_GetAttrString(py_file_obj, "write");
  PyObject_CallFunction(write_method, "s#", data, length);
    
  // MGDTODO: Check NULL on failure
}

static void flush_png_data(png_structp png_ptr) {
  PyObject* py_file_obj = (PyObject*)png_get_io_ptr(png_ptr);
  PyObject* flush_method = PyObject_GetAttrString(py_file_obj, "write");
  if (flush_method) {
    PyObject_CallFunction(flush_method, "");
  }
}

// this code is heavily adapted from the paint license, which is in
// the file paint.license (BSD compatible) included in this
// distribution.  TODO, add license file to MANIFEST.in and CVS
Py::Object
RendererAgg::write_png(const Py::Tuple& args)
{
  _VERBOSE("RendererAgg::write_png");
  
  args.verify_length(1, 2);
  
  FILE *fp = NULL;
  Py::Object py_fileobj = Py::Object(args[0]);
  if (py_fileobj.isString()) {
    std::string fileName = Py::String(py_fileobj);
    const char *file_name = fileName.c_str();
    if ((fp = fopen(file_name, "wb")) == NULL)
      throw Py::RuntimeError( Printf("Could not open file %s", file_name).str() );
  }
  else {
    if ((fp = PyFile_AsFile(py_fileobj.ptr())) == NULL) {
      PyObject* write_method = PyObject_GetAttrString(py_fileobj.ptr(), "write");
      if (!(write_method && PyCallable_Check(write_method)))
	throw Py::TypeError("Object does not appear to be a Python file-like object");
    }
  }

  png_bytep *row_pointers = NULL;
  png_structp png_ptr = NULL;
  png_infop info_ptr = NULL;
  
  try {
    struct        png_color_8_struct sig_bit;
    png_uint_32 row;
    
    row_pointers = new png_bytep[height];
    for (row = 0; row < height; ++row) {
      row_pointers[row] = pixBuffer + row * width * 4;
    }
  
    png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (png_ptr == NULL) {
      throw Py::RuntimeError("Could not create write struct");
    }
  
    info_ptr = png_create_info_struct(png_ptr);
    if (info_ptr == NULL) {
      throw Py::RuntimeError("Could not create info struct");
    }
  
    if (setjmp(png_ptr->jmpbuf)) {
      throw Py::RuntimeError("Error building image");
    }
  
    if (fp) {
      png_init_io(png_ptr, fp);
    } else {
      png_set_write_fn(png_ptr, (void*)py_fileobj.ptr(), 
		       &write_png_data, &flush_png_data);
    }
    png_set_IHDR(png_ptr, info_ptr,
		 width, height, 8,
		 PNG_COLOR_TYPE_RGB_ALPHA, PNG_INTERLACE_NONE,
		 PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);

    // Save the dpi of the image in the file
    if (args.size() == 2) {
      double dpi = Py::Float(args[1]);
      size_t dots_per_meter = (size_t)(dpi / (2.54 / 100.0));
      png_set_pHYs(png_ptr, info_ptr, dots_per_meter, dots_per_meter, PNG_RESOLUTION_METER);
    }
    
    // this a a color image!
    sig_bit.gray = 0;
    sig_bit.red = 8;
    sig_bit.green = 8;
    sig_bit.blue = 8;
    /* if the image has an alpha channel then */
    sig_bit.alpha = 8;
    png_set_sBIT(png_ptr, info_ptr, &sig_bit);
    
    png_write_info(png_ptr, info_ptr);
    png_write_image(png_ptr, row_pointers);
    png_write_end(png_ptr, info_ptr);
    
    /* Changed calls to png_destroy_write_struct to follow
       http://www.libpng.org/pub/png/libpng-manual.txt.
       This ensures the info_ptr memory is released.
    */
    
  } catch (...) {
      if (fp) fclose(fp);
      delete [] row_pointers;
      if (png_ptr && info_ptr) png_destroy_write_struct(&png_ptr, &info_ptr);
      throw;
  }
  
  png_destroy_write_struct(&png_ptr, &info_ptr);
  delete [] row_pointers;
  if (fp) fclose(fp);

  return Py::Object();
}


Py::Object
RendererAgg::tostring_rgb(const Py::Tuple& args) {
  //"Return the rendered buffer as an RGB string";
  
  _VERBOSE("RendererAgg::tostring_rgb");
  
  args.verify_length(0);
  int row_len = width*3;
  unsigned char* buf_tmp = new unsigned char[row_len * height];
  if (buf_tmp ==NULL) {
    //todo: also handle allocation throw
    throw Py::MemoryError("RendererAgg::tostring_rgb could not allocate memory");
  }
  agg::rendering_buffer renderingBufferTmp;
  renderingBufferTmp.attach(buf_tmp,
			    width,
			    height,
			    row_len);
  
  agg::color_conv(&renderingBufferTmp, renderingBuffer, agg::color_conv_rgba32_to_rgb24());
  
  
  //todo: how to do this with native CXX
  PyObject* o = Py_BuildValue("s#",
			      buf_tmp,
			      row_len * height);
  delete [] buf_tmp;
  return Py::asObject(o);
}


Py::Object
RendererAgg::tostring_argb(const Py::Tuple& args) {
  //"Return the rendered buffer as an RGB string";
  
  _VERBOSE("RendererAgg::tostring_argb");
  
  args.verify_length(0);
  int row_len = width*4;
  unsigned char* buf_tmp = new unsigned char[row_len * height];
  if (buf_tmp ==NULL) {
    //todo: also handle allocation throw
    throw Py::MemoryError("RendererAgg::tostring_argb could not allocate memory");
  }
  agg::rendering_buffer renderingBufferTmp;
  renderingBufferTmp.attach(buf_tmp,
			    width,
			    height,
			    row_len);
  
  agg::color_conv(&renderingBufferTmp, renderingBuffer, agg::color_conv_rgba32_to_argb32());
  
  
  //todo: how to do this with native CXX
  PyObject* o = Py_BuildValue("s#",
			      buf_tmp,
			      row_len * height);
  delete [] buf_tmp;
  return Py::asObject(o);
}

Py::Object
RendererAgg::tostring_bgra(const Py::Tuple& args) {
  //"Return the rendered buffer as an RGB string";
  
  _VERBOSE("RendererAgg::tostring_bgra");
  
  args.verify_length(0);
  int row_len = width*4;
  unsigned char* buf_tmp = new unsigned char[row_len * height];
  if (buf_tmp ==NULL) {
    //todo: also handle allocation throw
    throw Py::MemoryError("RendererAgg::tostring_bgra could not allocate memory");
  }
  agg::rendering_buffer renderingBufferTmp;
  renderingBufferTmp.attach(buf_tmp,
			    width,
			    height,
			    row_len);
  
  agg::color_conv(&renderingBufferTmp, renderingBuffer, agg::color_conv_rgba32_to_bgra32());
  
  
  //todo: how to do this with native CXX
  PyObject* o = Py_BuildValue("s#",
			      buf_tmp,
			      row_len * height);
  delete [] buf_tmp;
  return Py::asObject(o);
}

Py::Object
RendererAgg::buffer_rgba(const Py::Tuple& args) {
  //"expose the rendered buffer as Python buffer object, starting from postion x,y";
  
  _VERBOSE("RendererAgg::buffer_rgba");
  
  args.verify_length(2);
  int startw = Py::Int(args[0]);
  int starth = Py::Int(args[1]);
  int row_len = width*4;
  int start=row_len*starth+startw*4;
  return Py::asObject(PyBuffer_FromMemory( pixBuffer+start, row_len*height-start));
}



Py::Object
RendererAgg::clear(const Py::Tuple& args) {
  //"clear the rendered buffer";
  
  _VERBOSE("RendererAgg::clear");
  
  args.verify_length(0);
  rendererBase->clear(agg::rgba(1, 1, 1, 0));
  
  return Py::Object();
}


agg::rgba
RendererAgg::rgb_to_color(const Py::SeqBase<Py::Object>& rgb, double alpha) {
  _VERBOSE("RendererAgg::rgb_to_color");
  
  double r = Py::Float(rgb[0]);
  double g = Py::Float(rgb[1]);
  double b = Py::Float(rgb[2]);
  return agg::rgba(r, g, b, alpha);
  
}


double
RendererAgg::points_to_pixels_snapto(const Py::Object& points) {
  // convert a value in points to pixels depending on renderer dpi and
  // screen pixels per inch
  // snap return pixels to grid
  _VERBOSE("RendererAgg::points_to_pixels_snapto");
  double p = Py::Float( points ) ;
  //return (int)(p*PIXELS_PER_INCH/72.0*dpi/72.0)+0.5;
  return (int)(p*dpi/72.0)+0.5;
  
  
}

double
RendererAgg::points_to_pixels( const Py::Object& points) {
  _VERBOSE("RendererAgg::points_to_pixels");
  double p = Py::Float( points ) ;
  //return p * PIXELS_PER_INCH/72.0*dpi/72.0;
  return p * dpi/72.0;
}


RendererAgg::~RendererAgg() {
  
  _VERBOSE("RendererAgg::~RendererAgg");
  
  
  delete slineP8;
  delete slineBin;
  delete theRasterizer;
  delete rendererAA;
  delete rendererBin;
  delete rendererBase;
  delete pixFmt;
  delete renderingBuffer;
  
  delete alphaMask;
  delete alphaMaskRenderingBuffer;
  delete [] alphaBuffer;
  delete [] pixBuffer;
  delete pixfmtAlphaMask;
  delete rendererBaseAlphaMask;
  delete rendererAlphaMask;
  delete scanlineAlphaMask;
  
}

/* ------------ module methods ------------- */
Py::Object _backend_agg_module::new_renderer (const Py::Tuple &args,
					      const Py::Dict &kws)
{
  
  if (args.length() != 3 )
    {
      throw Py::RuntimeError("Incorrect # of args to RendererAgg(width, height, dpi).");
    }
  
  int debug;
  if ( kws.hasKey("debug") ) debug = Py::Int( kws["debug"] );
  else debug=0;
  
  int width = Py::Int(args[0]);
  int height = Py::Int(args[1]);
  double dpi = Py::Float(args[2]);
  return Py::asObject(new RendererAgg(width, height, dpi, debug));
}


void BufferRegion::init_type() {
  behaviors().name("BufferRegion");
  behaviors().doc("A wrapper to pass agg buffer objects to and from the python level");
  
  add_varargs_method("to_string", &BufferRegion::to_string,
		     "to_string()");
  
}


void RendererAgg::init_type()
{
  behaviors().name("RendererAgg");
  behaviors().doc("The agg backend extension module");
  
  add_varargs_method("draw_path", &RendererAgg::draw_path,
		     "draw_path(gc, path, transform, rgbFace)\n");
  add_varargs_method("draw_path_collection", &RendererAgg::draw_path_collection,
		     "draw_path_collection(master_transform, cliprect, clippath, clippath_trans, paths, transforms, offsets, offsetTrans, facecolors, edgecolors, linewidths, linestyles, antialiaseds)\n");
  add_varargs_method("draw_quad_mesh", &RendererAgg::draw_quad_mesh,
		     "draw_quad_mesh(master_transform, cliprect, clippath, clippath_trans, meshWidth, meshHeight, coordinates, offsets, offsetTrans, facecolors, antialiaseds, showedges)\n");
  add_varargs_method("draw_markers", &RendererAgg::draw_markers,
		     "draw_markers(gc, marker_path, marker_trans, path, rgbFace)\n");
  add_varargs_method("draw_text_image", &RendererAgg::draw_text_image,
		     "draw_text_image(font_image, x, y, r, g, b, a)\n");
  add_varargs_method("draw_image", &RendererAgg::draw_image,
		     "draw_image(x, y, im)");
  add_varargs_method("write_rgba", &RendererAgg::write_rgba,
		     "write_rgba(fname)");
  add_varargs_method("write_png", &RendererAgg::write_png,
		     "write_png(fname, dpi=None)");
  add_varargs_method("tostring_rgb", &RendererAgg::tostring_rgb,
		     "s = tostring_rgb()");
  add_varargs_method("tostring_argb", &RendererAgg::tostring_argb,
		     "s = tostring_argb()");
  add_varargs_method("tostring_bgra", &RendererAgg::tostring_bgra,
		     "s = tostring_bgra()");
  add_varargs_method("buffer_rgba", &RendererAgg::buffer_rgba,
		     "buffer = buffer_rgba()");
  add_varargs_method("clear", &RendererAgg::clear,
		     "clear()");
  add_varargs_method("copy_from_bbox", &RendererAgg::copy_from_bbox,
		     "copy_from_bbox(bbox)");
  
  add_varargs_method("restore_region", &RendererAgg::restore_region,
		     "restore_region(region)");
}

extern "C"
DL_EXPORT(void)
  init_backend_agg(void)
{
  //static _backend_agg_module* _backend_agg = new _backend_agg_module;
  
  _VERBOSE("init_backend_agg");
  
  import_array();
  
  static _backend_agg_module* _backend_agg = NULL;
  _backend_agg = new _backend_agg_module;
  
};
