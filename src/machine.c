/*
  Waveshaper effect for Buzztrax
  Copyright (C) 2020 David Beswick

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
  
  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "config.h"
#include "src/debug.h"
#include "src/properties_simple.h"

#include <gst/audio/audio-format.h>
#include <gst/base/gstbasetransform.h>

#include <math.h>

GST_DEBUG_CATEGORY(GST_CAT_DEFAULT);

G_DECLARE_FINAL_TYPE(BtEdbWaveshaper, btedb_waveshaper, BTEDB, WAVESHAPER, GstBaseTransform);

enum { MAX_ORDER = 10 };

struct _BtEdbWaveshaper
{
  GstBaseTransform parent;

  gfloat index;
  guint order;
  gint scalepowbase;
  gint scalepowexpoffset;
  gint scaleoffset;
  gfloat gain;
  
  gint sample_rate;
  gint channels;
  gfloat buf[MAX_ORDER+1];
  BtEdbPropertiesSimple* props;
  
  gint perf_samples;
  gulong perf_time;
};

G_DEFINE_TYPE(BtEdbWaveshaper, btedb_waveshaper, GST_TYPE_BASE_TRANSFORM);

static gboolean plugin_init(GstPlugin* plugin) {
  GST_DEBUG_CATEGORY_INIT(
    GST_CAT_DEFAULT,
    G_STRINGIFY(GST_CAT_DEFAULT),
    GST_DEBUG_FG_WHITE | GST_DEBUG_BG_BLACK,
    GST_MACHINE_DESC);

  return gst_element_register(
    plugin,
    G_STRINGIFY(GST_MACHINE_NAME),
    GST_RANK_NONE,
    btedb_waveshaper_get_type());
}

GST_PLUGIN_DEFINE(
  GST_VERSION_MAJOR,
  GST_VERSION_MINOR,
  GST_PLUGIN_NAME,
  GST_PLUGIN_DESC,
  plugin_init, VERSION, "GPL", PACKAGE_NAME, PACKAGE_BUGREPORT)

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-raw, "
        "format = (string) " GST_AUDIO_NE (F32) ", "
        "layout = (string) interleaved, "
        "rate = (int) [1, MAX], "
        "channels = (int) 1")
    );

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-raw, "
        "format = (string) " GST_AUDIO_NE (F32) ", "
        "layout = (string) interleaved, "
        "rate = (int) [1, MAX], "
        "channels = (int) 1")
    );

static void set_property (GObject* object, guint prop_id, const GValue* value, GParamSpec* pspec) {
  BtEdbWaveshaper* self = (BtEdbWaveshaper*)object;
  btedb_properties_simple_set(self->props, pspec, value);
}

static void get_property (GObject * object, guint prop_id, GValue * value, GParamSpec * pspec) {
  BtEdbWaveshaper* self = (BtEdbWaveshaper*)object;
  btedb_properties_simple_get(self->props, pspec, value);
}

static gboolean set_caps (GstBaseTransform* base, GstCaps* incaps, GstCaps* outcaps) {
  const GstStructure* structure = gst_caps_get_structure (incaps, 0);

  BtEdbWaveshaper* self = (BtEdbWaveshaper*)base;
  return gst_structure_get_int(structure, "rate", &self->sample_rate)
    && gst_structure_get_int(structure, "channels", &self->channels);
}

static GstFlowReturn transform_ip(GstBaseTransform* baset, GstBuffer* gstbuf) {
  struct timespec clock_start;
  clock_gettime(CLOCK_MONOTONIC_RAW, &clock_start);

  BtEdbWaveshaper* const self = (BtEdbWaveshaper*)baset;
  
  GstMapInfo info;
  
  if (!gst_buffer_map(gstbuf, &info, GST_MAP_READ | GST_MAP_WRITE)) {
    GST_ERROR_OBJECT(self, "unable to map buffer for read & write");
    return GST_FLOW_ERROR;
  }
  
  gfloat* data = (gfloat*)info.data;
  guint nsamples = info.size / sizeof(typeof(*data));
  const gfloat gain = 1.f/(self->order+1) * self->gain;

  for (int i = 0; i < nsamples; i++) {
    for (guint j = 0; j < self->order; ++j) {
      self->buf[j] = self->buf[j+1];
    }
    self->buf[self->order] = data[i];
    
    gfloat out = 0.f;
    for (guint j = 1; j <= self->order; ++j) {
      gfloat scale = (self->scaleoffset + powf(self->scalepowbase,j+self->scalepowexpoffset));
      if (scale)
        out += powf(self->index*self->buf[j], j) * scale;
    }

    out /= self->order+1;
    
    data[i] = out * gain;
  }
   
  gst_buffer_unmap (gstbuf, &info);

  struct timespec clock_end;
  clock_gettime(CLOCK_MONOTONIC_RAW, &clock_end);
  self->perf_time += (clock_end.tv_sec - clock_start.tv_sec) * 1e9L + (clock_end.tv_nsec - clock_start.tv_nsec);
  self->perf_samples += nsamples;
  if (self->perf_samples >= self->sample_rate) {
    GST_INFO("Avg perf: %f samples/sec\n", self->perf_samples / (self->perf_time / 1e9f));
    self->perf_time = 0;
    self->perf_samples = 0;
  }
  
  return GST_FLOW_OK;
}


static void btedb_waveshaper_init(BtEdbWaveshaper* const self) {
  self->props = btedb_properties_simple_new((GObject*)self);
  btedb_properties_simple_add(self->props, "index", &self->index);
  btedb_properties_simple_add(self->props, "order", &self->order);
  btedb_properties_simple_add(self->props, "scalepowbase", &self->scalepowbase);
  btedb_properties_simple_add(self->props, "scalepowexpoffset", &self->scalepowexpoffset);
  btedb_properties_simple_add(self->props, "scaleoffset", &self->scaleoffset);
  btedb_properties_simple_add(self->props, "gain", &self->gain);
}

static void dispose(GObject* object) {
  BtEdbWaveshaper* self = (BtEdbWaveshaper*)object;
  g_clear_object(&self->props);
}

static void btedb_waveshaper_class_init(BtEdbWaveshaperClass* const klass) {
  {
    GObjectClass* const aclass = (GObjectClass*)klass;
    aclass->set_property = set_property;
    aclass->get_property = get_property;
    aclass->dispose = dispose;

    const GParamFlags flags =
      (GParamFlags)(G_PARAM_READWRITE | GST_PARAM_CONTROLLABLE | G_PARAM_STATIC_STRINGS);

    guint idx = 1;
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("index", "Index", "Index", FLT_MIN, 10, 1, flags));
    
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_uint("order", "Order", "Order", 0, 10, 1, flags));

    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_int("scalepowbase", "Sc. Pow Base", "Scale Power Base", -10, 10, -1, flags));
    
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_int("scalepowexpoffset", "Sc. Pow Exp Off", "Sclae Power Exponent Offset", -5, 5, -1, flags));
    
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_int("scaleoffset", "Sc. Offset", "Scale Offset", -10, 10, 1, flags));

    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("gain", "Gain", "Gain", 0, 1, 1, flags));
  }

  {
    GstElementClass* const aclass = (GstElementClass*)klass;
    gst_element_class_set_static_metadata(
      aclass,
      G_STRINGIFY(GST_MACHINE_NAME),
      GST_MACHINE_CATEGORY,
      GST_MACHINE_DESC,
      PACKAGE_BUGREPORT);
    
    gst_element_class_add_pad_template(aclass, gst_static_pad_template_get(&src_template));
    gst_element_class_add_pad_template(aclass, gst_static_pad_template_get(&sink_template));
    
    // TBD: docs
    /*gst_element_class_add_metadata (element_class, GST_ELEMENT_METADATA_DOC_URI,
    "file://" DATADIR "" G_DIR_SEPARATOR_S "gtk-doc" G_DIR_SEPARATOR_S "html"
    G_DIR_SEPARATOR_S "" PACKAGE "-gst" G_DIR_SEPARATOR_S "GstBtSimSyn.html");*/
  }

  {
    GstBaseTransformClass* aclass = (GstBaseTransformClass*)klass;
    aclass->set_caps = set_caps;
//    aclass->start = GST_DEBUG_FUNCPTR (gstbt_audio_delay_start);
    aclass->transform_ip = transform_ip;
//    aclass->stop = GST_DEBUG_FUNCPTR (gstbt_audio_delay_stop);
  }
}

