// After Effects effect plugin: "Vector Motion Blur" (RSMB-style).
//
// Checks out the layer at t-1 frame, t, t+1 frame, converts to float, runs the
// shared core (optical flow + vector blur) and converts back.
// Supports 8 and 16 bpc. NOTE: written against the AE SDK but not compiled in
// the repo's CI; build it with -DAE_SDK_DIR=... (see README).

#include "AEConfig.h"
#include "entry.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_Macros.h"
#include "Param_Utils.h"

#include <algorithm>
#include <cmath>

#include "motion_blur.h"

enum {
    PARAM_INPUT = 0,
    PARAM_SHUTTER_ANGLE,
    PARAM_SHUTTER_PHASE,
    PARAM_BLUR_SCALE,
    PARAM_SAMPLES,
    PARAM_SMOOTHNESS,
    PARAM_EDGE_AWARE,
    PARAM_SHOW_VECTORS,
    PARAM_COUNT
};

// Must match AE_Effect_Global_OutFlags in MotionBlur.r.
static const A_long kOutFlags = PF_OutFlag_DEEP_COLOR_AWARE | PF_OutFlag_WIDE_TIME_INPUT;  // 0x02000002

static PF_Err About(PF_InData* in_data, PF_OutData* out, PF_ParamDef*[], PF_LayerDef*) {
    PF_SPRINTF(out->return_msg, "Vector Motion Blur 1.0\rOptical-flow based motion blur.");
    return PF_Err_NONE;
}

static PF_Err GlobalSetup(PF_InData*, PF_OutData* out, PF_ParamDef*[], PF_LayerDef*) {
    out->my_version = PF_VERSION(1, 0, 0, PF_Stage_DEVELOP, 0);
    out->out_flags = kOutFlags;
    out->out_flags2 = 0;
    return PF_Err_NONE;
}

static PF_Err ParamsSetup(PF_InData* in_data, PF_OutData* out, PF_ParamDef*[], PF_LayerDef*) {  // macros need the name `in_data`
    PF_ParamDef def;
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Shutter Angle", 0, 720, 0, 360, 180, PF_Precision_TENTHS, 0, 0, PARAM_SHUTTER_ANGLE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Shutter Phase", -360, 360, -180, 180, -90, PF_Precision_TENTHS, 0, 0, PARAM_SHUTTER_PHASE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Blur Amount %", 0, 1000, 0, 400, 100, PF_Precision_TENTHS, 0, 0, PARAM_BLUR_SCALE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Max Samples", 2, 256, 2, 128, 64, PARAM_SAMPLES);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Vector Smoothness", 0.01, 1.0, 0.02, 0.4, 0.1, PF_Precision_HUNDREDTHS, 0, 0, PARAM_SMOOTHNESS);
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOX("Edge-Aware Blur", "On", 0, 0, PARAM_EDGE_AWARE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOX("Motion Vectors", "Show", 0, 0, PARAM_SHOW_VECTORS);
    out->num_params = PARAM_COUNT;
    return PF_Err_NONE;
}

// ---- pixel conversion ------------------------------------------------------

static mb::Image toFloat(const PF_LayerDef* w) {
    mb::Image im(w->width, w->height);
    const bool deep = PF_WORLD_IS_DEEP(w);
    for (int y = 0; y < w->height; ++y) {
        const char* row = reinterpret_cast<const char*>(w->data) + size_t(y) * w->rowbytes;
        for (int x = 0; x < w->width; ++x) {
            float* d = im.at(x, y);
            if (deep) {
                const PF_Pixel16* p = reinterpret_cast<const PF_Pixel16*>(row) + x;
                const float k = 1.f / PF_MAX_CHAN16;
                d[0] = p->red * k; d[1] = p->green * k; d[2] = p->blue * k; d[3] = p->alpha * k;
            } else {
                const PF_Pixel8* p = reinterpret_cast<const PF_Pixel8*>(row) + x;
                const float k = 1.f / PF_MAX_CHAN8;
                d[0] = p->red * k; d[1] = p->green * k; d[2] = p->blue * k; d[3] = p->alpha * k;
            }
        }
    }
    return im;
}

static void fromFloat(const mb::Image& im, PF_LayerDef* w) {
    const bool deep = PF_WORLD_IS_DEEP(w);
    auto q = [](float v, float maxv) { return std::min(maxv, std::max(0.f, v * maxv) + 0.5f); };
    for (int y = 0; y < w->height; ++y) {
        char* row = reinterpret_cast<char*>(w->data) + size_t(y) * w->rowbytes;
        for (int x = 0; x < w->width; ++x) {
            const float* s = im.at(x, y);
            if (deep) {
                PF_Pixel16* p = reinterpret_cast<PF_Pixel16*>(row) + x;
                p->red = A_u_short(q(s[0], PF_MAX_CHAN16)); p->green = A_u_short(q(s[1], PF_MAX_CHAN16));
                p->blue = A_u_short(q(s[2], PF_MAX_CHAN16)); p->alpha = A_u_short(q(s[3], PF_MAX_CHAN16));
            } else {
                PF_Pixel8* p = reinterpret_cast<PF_Pixel8*>(row) + x;
                p->red = A_u_char(q(s[0], PF_MAX_CHAN8)); p->green = A_u_char(q(s[1], PF_MAX_CHAN8));
                p->blue = A_u_char(q(s[2], PF_MAX_CHAN8)); p->alpha = A_u_char(q(s[3], PF_MAX_CHAN8));
            }
        }
    }
}

// ---- render ---------------------------------------------------------------

static PF_Err Render(PF_InData* in, PF_OutData*, PF_ParamDef* params[], PF_LayerDef* output) {
    PF_Err err = PF_Err_NONE;
    const PF_LayerDef* src = &params[PARAM_INPUT]->u.ld;

    mb::Params p;
    p.shutterAngle = float(params[PARAM_SHUTTER_ANGLE]->u.fs_d.value);
    p.shutterPhase = float(params[PARAM_SHUTTER_PHASE]->u.fs_d.value);
    p.blurScale = float(params[PARAM_BLUR_SCALE]->u.fs_d.value) / 100.f;
    p.maxSamples = params[PARAM_SAMPLES]->u.sd.value;
    p.smoothness = float(params[PARAM_SMOOTHNESS]->u.fs_d.value);
    p.edgeAware = params[PARAM_EDGE_AWARE]->u.bd.value != 0;
    const bool showVectors = params[PARAM_SHOW_VECTORS]->u.bd.value != 0;

    // Neighbouring frames of the (already effect-processed) input layer.
    PF_ParamDef prevDef, nextDef;
    AEFX_CLR_STRUCT(prevDef);
    AEFX_CLR_STRUCT(nextDef);
    bool havePrev = false, haveNext = false;
    if (in->time_step != 0) {
        havePrev = !PF_CHECKOUT_PARAM(in, PARAM_INPUT, in->current_time - in->time_step, in->time_step,
                                      in->time_scale, &prevDef) && prevDef.u.ld.data;
        haveNext = !PF_CHECKOUT_PARAM(in, PARAM_INPUT, in->current_time + in->time_step, in->time_step,
                                      in->time_scale, &nextDef) && nextDef.u.ld.data;
    }

    mb::Image cur = toFloat(src), prev, next;
    // AE returns an empty/transparent layer outside the layer's duration; treat same-size only.
    auto usable = [&](const PF_ParamDef& d) {
        return d.u.ld.width == src->width && d.u.ld.height == src->height;
    };
    if (havePrev && usable(prevDef)) prev = toFloat(&prevDef.u.ld);
    if (haveNext && usable(nextDef)) next = toFloat(&nextDef.u.ld);

    mb::FlowField vel;
    mb::Image result = mb::motionBlur(cur, prev.w ? &prev : nullptr, next.w ? &next : nullptr, p, &vel);
    if (showVectors) {
        float maxMag = 1.f;
        for (size_t i = 0; i < vel.u.size(); ++i)
            maxMag = std::max(maxMag, std::sqrt(vel.u[i] * vel.u[i] + vel.v[i] * vel.v[i]));
        result = mb::flowToImage(vel, maxMag);
    }
    fromFloat(result, output);

    if (havePrev) PF_CHECKIN_PARAM(in, &prevDef);
    if (haveNext) PF_CHECKIN_PARAM(in, &nextDef);
    return err;
}

// SDK 27+: effects register themselves with the host via this entry point (in addition to the PiPL).
extern "C" DllExport PF_Err PluginDataEntryFunction2(PF_PluginDataPtr inPtr, PF_PluginDataCB2 inPluginDataCallBackPtr,
                                                    SPBasicSuite*, const char*, const char*) {
    PF_Err result = PF_Err_INVALID_CALLBACK;
    PF_REGISTER_EFFECT_EXT2(inPtr, inPluginDataCallBackPtr, "Vector Motion Blur",
                                     "PIPELIFE Vector Motion Blur", "Blur & Sharpen", AE_RESERVED_INFO,
                                     "EffectMain", "https://www.adobe.com");
    return result;
}

extern "C" DllExport PF_Err EffectMain(PF_Cmd cmd, PF_InData* in, PF_OutData* out, PF_ParamDef* params[],
                                      PF_LayerDef* output, void*) {
    try {
        switch (cmd) {
            case PF_Cmd_ABOUT:        return About(in, out, params, output);
            case PF_Cmd_GLOBAL_SETUP: return GlobalSetup(in, out, params, output);
            case PF_Cmd_PARAMS_SETUP: return ParamsSetup(in, out, params, output);
            case PF_Cmd_RENDER:       return Render(in, out, params, output);
            default: break;
        }
    } catch (const std::bad_alloc&) {
        return PF_Err_OUT_OF_MEMORY;
    } catch (...) {
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    return PF_Err_NONE;
}
