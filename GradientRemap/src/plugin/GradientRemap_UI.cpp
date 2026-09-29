// GradientRemap_UI.cpp -- custom gradient-bar UI (Phase 3, docs/DESIGN.md): draws the
// knot list live in the Effect Controls Window and handles click/drag editing, modelled
// directly on the AE SDK's ColorGrid (Drawbot draw pattern, AppSuite4 bg colour/colour-
// picker/invalidate) and Custom_ECW_UI (continue_refcon drag-gesture pattern) samples.
//
// No keyboard delete (2026-09-25): PF_Event_KEYDOWN's PF_InvalidateRect call errored
// with "internal verification failure ... can only be called during valid events" in
// real AE -- confirming the gap flagged when this was first built (no vendored SDK
// example exercises KEYDOWN reaching a custom-UI control region). Per user request,
// removed rather than debugged further: drag-off-the-bar deletion already covers the
// same need and is confirmed working.
//
// Geometry: a single coloured strip (kGradientBarHeight px) with pointer-and-swatch knot
// markers below it (kKnotMarkerHeight px) -- the conventional Photoshop/
// Cinema 4D gradient-editor layout. Hit-testing and drawing always use the real panel
// width from event_extra->effect_win.current_frame (the panel is resizable), not the
// nominal width hint passed to PF_ADD_ARBITRARY2.

#include "GradientRemap.h"
#include "GradientRemap_ColorSpace.h"

#include "../core/Interpolation.h"

#include <algorithm>
#include <cmath>

using GradientRemap::ClampUnit;
using GradientRemap::EvaluateGradient;
using GradientRemap::GradientData;
using GradientRemap::GradientKnot;
using GradientRemap::RGBAf;

namespace {

struct BarGeometry {
    A_long left;
    A_long top; // top of the coloured strip (below the margin)
    A_long width;
};

BarGeometry GetBarGeometry(const PF_EffectWindowInfo& effect_win) {
    BarGeometry g;
    g.left = effect_win.current_frame.left;
    g.top = effect_win.current_frame.top + kGradientUIMargin;
    g.width = effect_win.current_frame.right - effect_win.current_frame.left;
    if (g.width < 20) {
        g.width = kGradientBarWidth; // defensive fallback; shouldn't happen in practice
    }
    return g;
}

float KnotScreenX(const BarGeometry& bar, float knot_position) {
    return static_cast<float>(bar.left) + knot_position * static_cast<float>(bar.width);
}

float PositionFromScreenX(const BarGeometry& bar, float screen_x) {
    if (bar.width <= 0) return 0.0f;
    return ClampUnit((screen_x - static_cast<float>(bar.left)) / static_cast<float>(bar.width));
}

// Knot list from the arb param, with interpolation_mode/path overridden from the
// separate stock popups via the same mapping BuildGradientFromParams (GradientRemap_Main.cpp)
// uses -- this reads already-checked-out params[] directly (event handling never checks
// params in/out itself; AE hands them over already resolved for the current time).
GradientData BuildGradientForUI(PF_InData* in_data, PF_ParamDef* params[]) {
    GradientData g = GradientRemap_UnflattenArbHandle(in_data, params[GRADREMAP_GRADIENT]->u.arb_d.value);
    GradientRemap_ApplyInterpPopups(g, params[GRADREMAP_INTERP_MODE]->u.pd.value, params[GRADREMAP_PATH]->u.pd.value);
    return g;
}

GradientUISeqData* LockSeqData(PF_InData* in_data) {
    if (!in_data->sequence_data) return nullptr;
    auto* seq = reinterpret_cast<GradientUISeqData*>(PF_LOCK_HANDLE(in_data->sequence_data));
    if (seq && seq->magic != GRADIENT_UI_SEQ_MAGIC) {
        PF_UNLOCK_HANDLE(in_data->sequence_data);
        return nullptr; // shouldn't happen, but never trust garbage as selection state
    }
    return seq;
}

void UnlockSeqData(PF_InData* in_data) {
    if (in_data->sequence_data) PF_UNLOCK_HANDLE(in_data->sequence_data);
}

int HitTestKnot(const GradientData& g, const BarGeometry& bar, float mouse_h) {
    for (size_t i = 0; i < g.knots.size(); ++i) {
        float x = KnotScreenX(bar, g.knots[i].position);
        if (std::fabs(mouse_h - x) <= kKnotHitHalfWidth) return static_cast<int>(i);
    }
    return -1;
}

// After inserting a brand-new knot or repositioning an existing one to `position`, this
// counts how many OTHER knots (excluding `exclude_index`, or -1 if the knot is new and
// not yet in the list) sit strictly before it -- exactly where it lands after
// GradientData::SortKnots()'s *stable* sort (ties keep insertion order, and the knot
// being placed is always the last thing to move into that slot). Shared by DoClick's
// new-knot insertion and DoDrag's reposition/reorder.
int IndexAfterSortedInsert(const std::vector<GradientKnot>& knots, int exclude_index, float position) {
    int index = 0;
    for (size_t i = 0; i < knots.size(); ++i) {
        if (static_cast<int>(i) != exclude_index && knots[i].position < position) ++index;
    }
    return index;
}

bool PointInBar(const BarGeometry& bar, float mouse_h, float mouse_v) {
    return mouse_h >= static_cast<float>(bar.left) && mouse_h <= static_cast<float>(bar.left + bar.width) &&
           mouse_v >= static_cast<float>(bar.top) &&
           mouse_v <= static_cast<float>(bar.top + kGradientBarHeight + kKnotMarkerHeight);
}

// After Effects hands effects the project's own panel background colour as 16-bit/chan
// (0-65535); Drawbot wants 0-1 float. Matches ColorGrid's QDtoDRAWBOTColor exactly.
DRAWBOT_ColorRGBA AppColorToDrawbot(const PF_App_Color& c) {
    constexpr float kInv65535 = 1.0f / 65535.0f;
    DRAWBOT_ColorRGBA out;
    out.red = c.red * kInv65535;
    out.green = c.green * kInv65535;
    out.blue = c.blue * kInv65535;
    out.alpha = 1.0f;
    return out;
}

PF_Err AcquireBackgroundColor(PF_InData* in_data, PF_OutData* out_data, DRAWBOT_ColorRGBA* out_color) {
    PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;
    PFAppSuite4* app_suiteP = nullptr;
    PF_App_Color local_color = {0, 0, 0};

    ERR(AEFX_AcquireSuite(in_data, out_data, kPFAppSuite, kPFAppSuiteVersion4, NULL, (void**)&app_suiteP));
    if (app_suiteP) {
        ERR(app_suiteP->PF_AppGetBgColor(&local_color));
        if (!err && out_color) {
            *out_color = AppColorToDrawbot(local_color);
        }
    }
    ERR2(AEFX_ReleaseSuite(in_data, out_data, kPFAppSuite, kPFAppSuiteVersion4, NULL));
    return err;
}

// Mirrors the selected knot into the "Knot Position" field (0-100%). The value goes
// through PF_ChangeFlag_CHANGED_VALUE (the same route the arb param already uses from
// these events); PF_UpdateParamUI pushes it to the field's display and keeps its slider
// twirl-down collapsed. The field is never greyed out (2026-09-29, user feedback: going
// grey only after a deletion felt inconsistent); with nothing selected it just keeps its
// last value, and editing it does nothing (see GradientRemap_HandleKnotPositionChanged).
// Only touches the field when the value actually differs, so merely re-clicking the
// selected knot doesn't add an undo step.
PF_Err SyncKnotPositionControl(PF_InData* in_data, PF_ParamDef* params[], const GradientData& g, A_long selected) {
    if (selected < 0 || selected >= static_cast<A_long>(g.knots.size())) return PF_Err_NONE;

    PF_ParamDef* field = params[GRADREMAP_KNOT_POSITION];
    PF_FpLong wanted = static_cast<PF_FpLong>(g.knots[static_cast<size_t>(selected)].position) * 100.0;
    if (std::fabs(field->u.fs_d.value - wanted) <= 1e-6) return PF_Err_NONE;

    field->u.fs_d.value = wanted;
    field->uu.change_flags |= PF_ChangeFlag_CHANGED_VALUE;

    PF_ParamDef updated = *field; // PF_UpdateParamUI wants a copy, not the live param
    updated.flags |= PF_ParamFlag_COLLAPSE_TWIRLY; // keep the slider bar hidden: number only
    updated.ui_flags &= ~PF_PUI_DISABLED;          // clear any greyed state from earlier builds
    AEGP_SuiteHandler suites(in_data->pica_basicP);
    return suites.ParamUtilsSuite3()->PF_UpdateParamUI(in_data->effect_ref, GRADREMAP_KNOT_POSITION, &updated);
}

PF_Err InvalidateWholeControl(PF_InData* in_data, PF_OutData* out_data, PF_EventExtra* event_extra) {
    PF_Err err = PF_Err_NONE;
    AEGP_SuiteHandler suites(in_data->pica_basicP);
    PF_Rect inval(event_extra->effect_win.current_frame);
    ERR(suites.AppSuite4()->PF_InvalidateRect(event_extra->contextH, &inval));
    event_extra->evt_out_flags |= PF_EO_HANDLED_EVENT | PF_EO_UPDATE_NOW;
    return err;
}

PF_Err DrawEvent(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_EventExtra* event_extra) {
    PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;
    if (event_extra->effect_win.area != PF_EA_CONTROL) return err;

    DRAWBOT_Suites drawbotSuites;
    ERR(AEFX_AcquireDrawbotSuites(in_data, out_data, &drawbotSuites));

    DRAWBOT_DrawRef drawing_ref = nullptr;
    PF_EffectCustomUISuite1* customUISuiteP = nullptr;
    if (!err) {
        err = AEFX_AcquireSuite(in_data, out_data, kPFEffectCustomUISuite, kPFEffectCustomUISuiteVersion1, NULL,
                                 (void**)&customUISuiteP);
        if (!err && customUISuiteP) {
            err = (*customUISuiteP->PF_GetDrawingReference)(event_extra->contextH, &drawing_ref);
            AEFX_ReleaseSuite(in_data, out_data, kPFEffectCustomUISuite, kPFEffectCustomUISuiteVersion1, NULL);
        }
    }

    DRAWBOT_SupplierRef supplier_ref = nullptr;
    DRAWBOT_SurfaceRef surface_ref = nullptr;
    if (!err) {
        ERR(drawbotSuites.drawbot_suiteP->GetSupplier(drawing_ref, &supplier_ref));
        ERR(drawbotSuites.drawbot_suiteP->GetSurface(drawing_ref, &surface_ref));
    }

    DRAWBOT_ColorRGBA bg_color = {0, 0, 0, 1};
    ERR(AcquireBackgroundColor(in_data, out_data, &bg_color));

    if (!err) {
        BarGeometry bar = GetBarGeometry(event_extra->effect_win);

        DRAWBOT_RectF32 full_rect;
        full_rect.left = (float)event_extra->effect_win.current_frame.left;
        full_rect.top = (float)event_extra->effect_win.current_frame.top;
        full_rect.width = (float)bar.width;
        full_rect.height = (float)kGradientUITotalHeight;
        ERR(drawbotSuites.surface_suiteP->PaintRect(surface_ref, &bg_color, &full_rect));
    }

    if (!err) {
        BarGeometry bar = GetBarGeometry(event_extra->effect_win);
        GradientData g = BuildGradientForUI(in_data, params);

        if (g.IsValid()) {
            float gamma = GradientRemap_QueryWorkingSpaceGamma(in_data);

            // Gradient strip: sample and paint one 1px-wide column at a time -- Drawbot
            // has no built-in gradient-fill primitive (confirmed via header search).
            for (A_long col = 0; col < bar.width && !err; ++col) {
                float t = static_cast<float>(col) / static_cast<float>(bar.width > 1 ? bar.width - 1 : 1);
                RGBAf c = EvaluateGradient(g, t, gamma);
                DRAWBOT_ColorRGBA stripColor = {ClampUnit(c.r), ClampUnit(c.g), ClampUnit(c.b), 1.0f};
                DRAWBOT_RectF32 colRect;
                colRect.left = (float)(bar.left + col);
                colRect.top = (float)bar.top;
                colRect.width = 1.0f;
                colRect.height = (float)kGradientBarHeight;
                ERR(drawbotSuites.surface_suiteP->PaintRect(surface_ref, &stripColor, &colRect));
            }

            GradientUISeqData* seq = LockSeqData(in_data);
            A_long selected = seq ? seq->selected_knot_index : -1;
            PF_Boolean delete_pending = seq ? seq->delete_pending : FALSE;
            UnlockSeqData(in_data);

            // Knot markers: a pointer tip touching the strip above a square swatch filled
            // with the knot's colour. Every marker carries a thin light-grey keyline so it
            // reads against both the dark panel and any fill colour; the selected knot's
            // keyline is thicker and white.
            constexpr DRAWBOT_ColorRGBA kKeylineGrey = {0.75f, 0.75f, 0.75f, 1.0f};
            constexpr DRAWBOT_ColorRGBA kKeylineSelected = {1.0f, 1.0f, 1.0f, 1.0f};
            for (size_t i = 0; i < g.knots.size() && !err; ++i) {
                bool is_selected = (static_cast<A_long>(i) == selected);
                bool fading = is_selected && delete_pending;
                float shrink = fading ? 0.6f : 1.0f;

                // Snap to the pixel centre so a 1px keyline renders crisp, not smeared
                // across two pixels.
                float x = std::floor(KnotScreenX(bar, g.knots[i].position)) + 0.5f;
                float apex_y = (float)(bar.top + kGradientBarHeight) + 0.5f;
                float half_w = kKnotMarkerHalfWidth * shrink;
                float shoulder_y = apex_y + (float)kKnotPointerHeight * shrink;
                float base_y = apex_y + (float)kKnotMarkerHeight * shrink - 1.0f;

                DRAWBOT_PathRef tri_path = nullptr;
                ERR(drawbotSuites.supplier_suiteP->NewPath(supplier_ref, &tri_path));
                ERR(drawbotSuites.path_suiteP->MoveTo(tri_path, x, apex_y));
                ERR(drawbotSuites.path_suiteP->LineTo(tri_path, x + half_w, shoulder_y));
                ERR(drawbotSuites.path_suiteP->LineTo(tri_path, x + half_w, base_y));
                ERR(drawbotSuites.path_suiteP->LineTo(tri_path, x - half_w, base_y));
                ERR(drawbotSuites.path_suiteP->LineTo(tri_path, x - half_w, shoulder_y));
                ERR(drawbotSuites.path_suiteP->Close(tri_path));

                DRAWBOT_ColorRGBA fillColor = {ClampUnit(g.knots[i].r), ClampUnit(g.knots[i].g), ClampUnit(g.knots[i].b),
                                               fading ? 0.4f : 1.0f};
                DRAWBOT_BrushRef fill_brush = nullptr;
                ERR(drawbotSuites.supplier_suiteP->NewBrush(supplier_ref, &fillColor, &fill_brush));
                ERR(drawbotSuites.surface_suiteP->FillPath(surface_ref, fill_brush, tri_path, kDRAWBOT_FillType_Default));
                if (fill_brush) drawbotSuites.supplier_suiteP->ReleaseObject((DRAWBOT_ObjectRef)fill_brush);

                DRAWBOT_ColorRGBA outlineColor = is_selected ? kKeylineSelected : kKeylineGrey;
                DRAWBOT_PenRef outline_pen = nullptr;
                ERR(drawbotSuites.supplier_suiteP->NewPen(supplier_ref, &outlineColor, is_selected ? 2.0f : 1.0f,
                                                           &outline_pen));
                ERR(drawbotSuites.surface_suiteP->StrokePath(surface_ref, outline_pen, tri_path));
                if (outline_pen) drawbotSuites.supplier_suiteP->ReleaseObject((DRAWBOT_ObjectRef)outline_pen);

                if (tri_path) drawbotSuites.supplier_suiteP->ReleaseObject((DRAWBOT_ObjectRef)tri_path);
            }
        }
    }

    ERR2(AEFX_ReleaseDrawbotSuites(in_data, out_data));
    event_extra->evt_out_flags = PF_EO_HANDLED_EVENT;
    return err;
}

PF_Err DoClick(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_EventExtra* event_extra) {
    PF_Err err = PF_Err_NONE;
    if (event_extra->effect_win.area != PF_EA_CONTROL) return err;

    BarGeometry bar = GetBarGeometry(event_extra->effect_win);
    float mouse_h = (float)event_extra->u.do_click.screen_point.h;
    float mouse_v = (float)event_extra->u.do_click.screen_point.v;
    if (!PointInBar(bar, mouse_h, mouse_v)) return err; // click outside our control entirely -- not our event

    GradientData g = BuildGradientForUI(in_data, params);
    int hit = HitTestKnot(g, bar, mouse_h);
    A_u_long when = event_extra->u.do_click.when;

    GradientUISeqData* seq = LockSeqData(in_data);

    if (hit >= 0) {
        // Double-click detection: no dedicated AE event type exists for this. Hedge with
        // both num_clicks (the SDK-intended signal, unconfirmed cross-host reliability)
        // and a manual same-knot-quick-succession fallback via sequence data -- see
        // GradientRemap.h's kDoubleClickMaxWhenDelta comment.
        bool is_double_click = event_extra->u.do_click.num_clicks >= 2;
        if (!is_double_click && seq && seq->last_click_knot_index == hit &&
            (when - seq->last_click_when) <= kDoubleClickMaxWhenDelta) {
            is_double_click = true;
        }

        if (seq) {
            seq->selected_knot_index = hit;
            seq->last_click_knot_index = hit;
            seq->last_click_when = when;
            seq->delete_pending = FALSE;
        }

        if (is_double_click) {
            PF_PixelFloat sample;
            sample.red = g.knots[hit].r;
            sample.green = g.knots[hit].g;
            sample.blue = g.knots[hit].b;
            sample.alpha = 1.0f;

            AEGP_SuiteHandler suites(in_data->pica_basicP);
            if (in_data->appl_id != kAppID_Premiere) {
                PF_PixelFloat picked = sample;
                err = suites.AppSuite4()->PF_AppColorPickerDialog("Gradient Knot Colour", &sample, TRUE, &picked);
                if (!err) {
                    g.knots[hit].r = picked.red;
                    g.knots[hit].g = picked.green;
                    g.knots[hit].b = picked.blue;
                    err = GradientRemap_WriteGradientAndMarkChanged(in_data, params, g);
                }
            }
        } else {
            event_extra->u.do_click.continue_refcon[0] = (A_intptr_t)hit;
            event_extra->u.do_click.send_drag = TRUE;
        }
    } else {
        // Empty bar space: insert a new knot at the click position, seeded with the
        // gradient's own current colour there (matches Photoshop's "click to add a
        // stop" behaviour -- doesn't introduce a visible jump).
        float t = PositionFromScreenX(bar, mouse_h);
        RGBAf c = EvaluateGradient(g, t, GradientRemap_QueryWorkingSpaceGamma(in_data));

        int new_index = IndexAfterSortedInsert(g.knots, -1, t);

        GradientKnot k{};
        k.position = t;
        k.r = c.r;
        k.g = c.g;
        k.b = c.b;
        k.a = c.a;
        g.knots.push_back(k);
        g.SortKnots(); // stable; ties keep insertion order, so new_index is still correct

        err = GradientRemap_WriteGradientAndMarkChanged(in_data, params, g);

        if (seq) {
            seq->selected_knot_index = new_index;
            seq->last_click_knot_index = -1;
            seq->delete_pending = FALSE;
        }
        event_extra->u.do_click.continue_refcon[0] = (A_intptr_t)new_index;
        event_extra->u.do_click.send_drag = TRUE;
    }

    A_long selected = seq ? seq->selected_knot_index : -1;
    UnlockSeqData(in_data);

    if (!err) err = SyncKnotPositionControl(in_data, params, g, selected);

    PF_Err inval_err = InvalidateWholeControl(in_data, out_data, event_extra);
    return err ? err : inval_err;
}

PF_Err DoDrag(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_EventExtra* event_extra) {
    PF_Err err = PF_Err_NONE;
    int dragged_index = (int)event_extra->u.do_click.continue_refcon[0];

    GradientData g = BuildGradientForUI(in_data, params);
    if (dragged_index < 0 || dragged_index >= (int)g.knots.size()) return err;

    BarGeometry bar = GetBarGeometry(event_extra->effect_win);
    float mouse_h = (float)event_extra->u.do_click.screen_point.h;
    float mouse_v = (float)event_extra->u.do_click.screen_point.v;

    // Knots can freely slide past one another (reordering the gradient's colours) --
    // only clamped to the bar's own [0,1] extent, not to immediate neighbours.
    float t = PositionFromScreenX(bar, mouse_h);

    // Re-derive the dragged knot's index after the reposition: count how many OTHER
    // knots sit strictly before its new position `t` -- that count is exactly where it
    // lands after GradientData::SortKnots()'s *stable* sort (ties keep insertion order,
    // and the dragged knot is the last thing to move into that slot).
    int new_index = IndexAfterSortedInsert(g.knots, dragged_index, t);
    g.knots[dragged_index].position = t;
    g.SortKnots();
    dragged_index = new_index;
    event_extra->u.do_click.continue_refcon[0] = (A_intptr_t)dragged_index;

    // Vertical distance from the bar past kKnotDeleteDragDistance marks the knot for
    // deletion (visual fade only, until the gesture actually ends).
    float dist_above = (float)bar.top - mouse_v;
    float dist_below = mouse_v - (float)(bar.top + kGradientBarHeight + kKnotMarkerHeight);
    bool delete_pending = std::max(dist_above, dist_below) > kKnotDeleteDragDistance;

    bool last_time = event_extra->u.do_click.last_time;
    bool should_delete_now = last_time && delete_pending && g.knots.size() > GradientData::kMinKnots;
    if (should_delete_now) {
        g.knots.erase(g.knots.begin() + dragged_index);
    }

    err = GradientRemap_WriteGradientAndMarkChanged(in_data, params, g);

    GradientUISeqData* seq = LockSeqData(in_data);
    if (seq) {
        if (last_time) {
            seq->delete_pending = FALSE;
            // After a deletion, select the knot to its left (or the new first knot) so the
            // Knot Position field always refers to a live knot rather than the deleted one.
            seq->selected_knot_index = should_delete_now ? std::max(dragged_index - 1, 0) : dragged_index;
        } else {
            seq->selected_knot_index = dragged_index;
            seq->delete_pending = delete_pending;
        }
    }
    A_long selected = seq ? seq->selected_knot_index : -1;
    UnlockSeqData(in_data);

    if (!err) err = SyncKnotPositionControl(in_data, params, g, selected);

    PF_Err inval_err = InvalidateWholeControl(in_data, out_data, event_extra);
    return err ? err : inval_err;
}

PF_Err ChangeCursor(PF_InData* in_data, PF_ParamDef* params[], PF_EventExtra* event_extra) {
    if (event_extra->effect_win.area != PF_EA_CONTROL) return PF_Err_NONE;

    BarGeometry bar = GetBarGeometry(event_extra->effect_win);
    float mouse_h = (float)event_extra->u.adjust_cursor.screen_point.h;
    float mouse_v = (float)event_extra->u.adjust_cursor.screen_point.v;
    if (!PointInBar(bar, mouse_h, mouse_v)) return PF_Err_NONE;

    GradientData g = BuildGradientForUI(in_data, params);
    int hit = HitTestKnot(g, bar, mouse_h);
    event_extra->u.adjust_cursor.set_cursor = (hit >= 0) ? PF_Cursor_DRAG_DOT : PF_Cursor_FINGER_POINTER;
    return PF_Err_NONE;
}

} // namespace

PF_Err GradientRemap_HandleKnotPositionChanged(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[]) {
    GradientData g = GradientRemap_UnflattenArbHandle(in_data, params[GRADREMAP_GRADIENT]->u.arb_d.value);

    GradientUISeqData* seq = LockSeqData(in_data);
    A_long selected = seq ? seq->selected_knot_index : -1;
    if (selected < 0 || selected >= static_cast<A_long>(g.knots.size())) {
        UnlockSeqData(in_data);
        return PF_Err_NONE; // no knot selected yet (fresh or reopened effect); nothing to move
    }

    // Same free-reordering move as dragging (see DoDrag): the knot may pass its
    // neighbours, and the selection follows it to its post-sort index.
    float t = ClampUnit(static_cast<float>(params[GRADREMAP_KNOT_POSITION]->u.fs_d.value / 100.0));
    int new_index = IndexAfterSortedInsert(g.knots, static_cast<int>(selected), t);
    g.knots[static_cast<size_t>(selected)].position = t;
    g.SortKnots();
    seq->selected_knot_index = new_index;
    UnlockSeqData(in_data);

    PF_Err err = GradientRemap_WriteGradientAndMarkChanged(in_data, params, g);
    out_data->out_flags |= PF_OutFlag_REFRESH_UI; // redraw the bar with the moved knot
    return err;
}

PF_Err GradientRemap_HandleEvent(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[],
                                  PF_LayerDef* /*output*/, PF_EventExtra* extra) {
    PF_Err err = PF_Err_NONE;
    switch (extra->e_type) {
        case PF_Event_DRAW:
            err = DrawEvent(in_data, out_data, params, extra);
            break;
        case PF_Event_DO_CLICK:
            err = DoClick(in_data, out_data, params, extra);
            break;
        case PF_Event_DRAG:
            err = DoDrag(in_data, out_data, params, extra);
            break;
        case PF_Event_ADJUST_CURSOR:
            err = ChangeCursor(in_data, params, extra);
            break;
        default:
            break;
    }
    return err;
}

// Transient UI-thread-only selection state (see GradientRemap.h) -- never persisted,
// never touched by the render thread's own separate sequence_data copy (genuinely
// separate memory as of AE 13.5). Lifecycle mirrors the SDK's own HistoGrid sample.
PF_Err GradientRemap_SequenceSetup(PF_InData* in_data, PF_OutData* out_data) {
    PF_Handle h = PF_NEW_HANDLE(sizeof(GradientUISeqData));
    if (!h) return PF_Err_OUT_OF_MEMORY;

    auto* seq = reinterpret_cast<GradientUISeqData*>(PF_LOCK_HANDLE(h));
    if (seq) {
        seq->magic = GRADIENT_UI_SEQ_MAGIC;
        seq->selected_knot_index = -1;
        seq->last_click_knot_index = -1;
        seq->last_click_when = 0;
        seq->delete_pending = FALSE;
    }
    PF_UNLOCK_HANDLE(h);

    out_data->sequence_data = h;
    return PF_Err_NONE;
}

PF_Err GradientRemap_SequenceSetdown(PF_InData* in_data, PF_OutData* /*out_data*/) {
    if (in_data->sequence_data) {
        PF_DISPOSE_HANDLE(in_data->sequence_data);
    }
    return PF_Err_NONE;
}

PF_Err GradientRemap_SequenceResetup(PF_InData* in_data, PF_OutData* out_data) {
    if (in_data->sequence_data) {
        // Selection is transient: start unselected on reopen/duplicate (the "Knot
        // Position" field is control-only, so it doesn't keep its value either).
        GradientUISeqData* seq = LockSeqData(in_data);
        if (seq) seq->selected_knot_index = -1;
        UnlockSeqData(in_data);
        out_data->sequence_data = in_data->sequence_data;
        return PF_Err_NONE;
    }
    return GradientRemap_SequenceSetup(in_data, out_data);
}

PF_Err GradientRemap_SequenceFlatten(PF_InData* in_data, PF_OutData* out_data) {
    // GradientUISeqData is plain POD with no pointers, so it's already in a form safe to
    // write to disk as-is -- nothing to transform, matching HistoGrid's own handling of a
    // similarly-transient, pointer-free sequence struct.
    out_data->sequence_data = in_data->sequence_data;
    return PF_Err_NONE;
}
