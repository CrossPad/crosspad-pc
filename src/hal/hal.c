#include "hal.h"

#include <SDL.h>
#include <stdbool.h>

static lv_indev_t * s_encoder = NULL;
static lv_indev_t * s_mouse = NULL;
static lv_display_t * s_disp = NULL;

/* The mouse as the board's touch panel. Not LVGL's lv_sdl_mouse: its event
 * handler finds its indev by comparing read_cb against its own, so the
 * moment crosspad-gui's touch_gesture wraps read_cb -- as it does the panel's
 * on the board -- the mouse stops reaching LVGL at all. This one is fed from
 * an SDL event watch, which sees real mouse events when they are pumped and
 * the control port's SDL_PushEvent ones as they are pushed, and reads in
 * LVGL's event mode, one read per event, like the driver it replaces. */
static struct {
  int32_t x, y;
  bool down;
} s_touch;

static void touch_read(lv_indev_t * indev, lv_indev_data_t * data)
{
  (void)indev;
  data->point.x = s_touch.x;
  data->point.y = s_touch.y;
  data->state = s_touch.down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static int touch_watch(void * user, SDL_Event * e)
{
  (void)user;
  if (!s_mouse || !s_disp) return 0;
  /* An event watch runs on whichever thread pushes the event -- SDL's own
   * signal and hotplug paths included -- so nothing of LVGL is touched for an
   * event this driver does not take. The ones it takes come from the LVGL
   * thread: SDL_PollEvent() in the SDL driver's timer, and the control
   * port's SDL_PushEvent() inside remote::process_pending(). */
  if (e->type != SDL_MOUSEBUTTONDOWN && e->type != SDL_MOUSEBUTTONUP &&
      e->type != SDL_MOUSEMOTION && e->type != SDL_WINDOWEVENT) return 0;
#if LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR < 4
  /* A board's LVGL 9.3 (the browser twin) has no lv_sdl_window_get_window():
   * there the page's canvas is the one window, so every event is this one's. */
  const Uint32 win = e->type == SDL_WINDOWEVENT ? e->window.windowID
                   : e->type == SDL_MOUSEMOTION ? e->motion.windowID : e->button.windowID;
#else
  const Uint32 win = SDL_GetWindowID(lv_sdl_window_get_window(s_disp));
#endif
  const float zoom = lv_sdl_window_get_zoom(s_disp);
  switch (e->type) {
    case SDL_MOUSEBUTTONDOWN:
      if (e->button.windowID != win || e->button.button != SDL_BUTTON_LEFT) return 0;
      s_touch.x = (int32_t)(e->button.x / zoom);
      s_touch.y = (int32_t)(e->button.y / zoom);
      s_touch.down = true;
      break;
    case SDL_MOUSEBUTTONUP:
      if (e->button.windowID != win || e->button.button != SDL_BUTTON_LEFT) return 0;
      s_touch.down = false;
      break;
    case SDL_MOUSEMOTION:
      if (e->motion.windowID != win) return 0;
      s_touch.x = (int32_t)(e->motion.x / zoom);
      s_touch.y = (int32_t)(e->motion.y / zoom);
      break;
    case SDL_WINDOWEVENT:
      if (e->window.windowID != win || e->window.event != SDL_WINDOWEVENT_LEAVE) return 0;
      s_touch.down = false;
      break;
    default:
      return 0;
  }
  lv_indev_read(s_mouse);
  return 0;
}

lv_indev_t * sdl_hal_get_encoder(void)
{
  return s_encoder;
}

lv_indev_t * sdl_hal_get_mouse(void)
{
  return s_mouse;
}

lv_display_t * sdl_hal_init(int32_t w, int32_t h)
{

  lv_group_set_default(lv_group_create());

  lv_display_t * disp = lv_sdl_window_create(w, h);

  s_disp = disp;
  lv_indev_t * mouse = lv_indev_create();
  lv_indev_set_type(mouse, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(mouse, touch_read);
  lv_indev_set_mode(mouse, LV_INDEV_MODE_EVENT);
  s_mouse = mouse;
  SDL_AddEventWatch(touch_watch, NULL);
  lv_indev_set_group(mouse, lv_group_get_default());
  lv_indev_set_display(mouse, disp);
  lv_display_set_default(disp);
#ifndef __EMSCRIPTEN__   /* the page reads the LCD out as pixels: no cursor on it */
  /*Declare the image file.*/
  LV_IMAGE_DECLARE(mouse_cursor_icon); 
  lv_obj_t * cursor_obj;
  /*Create an image object for the cursor */
  cursor_obj = lv_image_create(lv_screen_active()); 
  /*Set the image source*/
  lv_image_set_src(cursor_obj, &mouse_cursor_icon);           
  /*Connect the image  object to the driver*/
  lv_indev_set_cursor(mouse, cursor_obj);
#endif             

  /* The rotary encoder. Left without a group on purpose: crosspad-gui hands it
   * one per screen (launcher, settings, the quick-settings drawer), the same
   * way indev_encoderTop is wired on the board. */
  s_encoder = lv_sdl_mousewheel_create();
  lv_indev_set_display(s_encoder, disp);

  lv_indev_t * kb = lv_sdl_keyboard_create();
  lv_indev_set_display(kb, disp);
  lv_indev_set_group(kb, lv_group_get_default());

  return disp;
}
