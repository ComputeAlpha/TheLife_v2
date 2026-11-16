#include "x11/xcb_source.h"

#include <sys/ipc.h>
#include <sys/shm.h>
#include <xcb/randr.h>
#include <xcb/res.h>
#include <xcb/screensaver.h>
#include <xcb/shm.h>
#include <xcb/xcb.h>

#include <cstdlib>
#include <cstring>

namespace thelife::collector {

std::optional<int> monitorOf(const std::vector<Monitor>& ms, const Rect& r) {
  const int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
  for (const auto& m : ms) {
    if (cx >= m.r.x && cx < m.r.x + m.r.w && cy >= m.r.y && cy < m.r.y + m.r.h) return m.index;
  }
  return std::nullopt;
}

namespace {

template <typename T>
struct Reply {
  T* p;
  explicit Reply(T* x) : p(x) {}
  ~Reply() { std::free(p); }
  T* operator->() const { return p; }
  explicit operator bool() const { return p != nullptr; }
};

std::string latin1ToUtf8(const std::string& s) {
  std::string out;
  for (unsigned char ch : s) {
    if (ch < 0x80) {
      out += static_cast<char>(ch);
    } else {
      out += static_cast<char>(0xC0 | (ch >> 6));
      out += static_cast<char>(0x80 | (ch & 0x3F));
    }
  }
  return out;
}

bool extension(xcb_connection_t* c, xcb_extension_t* ext) {
  const auto* d = xcb_get_extension_data(c, ext);
  return d && d->present;
}

}  // namespace

std::unique_ptr<XcbSource> XcbSource::connect(const char* display, std::string* err) {
  std::unique_ptr<XcbSource> s(new XcbSource());
  int screenNum = 0;
  s->c_ = xcb_connect(display, &screenNum);
  if (!s->c_ || xcb_connection_has_error(s->c_)) {
    if (err) *err = "cannot connect to the X display (is DISPLAY set?)";
    return nullptr;
  }
  s->display_ = display ? display : (std::getenv("DISPLAY") ? std::getenv("DISPLAY") : "");
  auto it = xcb_setup_roots_iterator(xcb_get_setup(s->c_));
  for (int i = 0; i < screenNum && it.rem; ++i) xcb_screen_next(&it);
  if (!it.rem) {
    if (err) *err = "X screen not found";
    return nullptr;
  }
  s->root_ = it.data->root;
  s->rootDepth_ = it.data->root_depth;
  s->haveShm_ = extension(s->c_, &xcb_shm_id);
  s->haveSaver_ = extension(s->c_, &xcb_screensaver_id);
  s->haveRandr_ = extension(s->c_, &xcb_randr_id);
  s->haveRes_ = extension(s->c_, &xcb_res_id);
  if (s->haveRandr_) {
    Reply v(xcb_randr_query_version_reply(s->c_, xcb_randr_query_version(s->c_, 1, 5), nullptr));
    s->haveRandr_ = v && (v->major_version > 1 || v->minor_version >= 5);
  }
  if (s->haveRes_) {
    Reply v(xcb_res_query_version_reply(s->c_, xcb_res_query_version(s->c_, 1, 2), nullptr));
    s->haveRes_ = v && (v->server_major > 1 || v->server_minor >= 2);
  }
  const uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
  xcb_change_window_attributes(s->c_, s->root_, XCB_CW_EVENT_MASK, &mask);
  xcb_flush(s->c_);
  return s;
}

XcbSource::~XcbSource() {
  if (c_) xcb_disconnect(c_);
}

int XcbSource::fd() const { return xcb_get_file_descriptor(c_); }
bool XcbSource::ok() const { return c_ && !xcb_connection_has_error(c_); }

uint32_t XcbSource::atom(const char* name) {
  if (const auto it = atoms_.find(name); it != atoms_.end()) return it->second;
  Reply r(xcb_intern_atom_reply(c_, xcb_intern_atom(c_, 0, static_cast<uint16_t>(std::strlen(name)), name), nullptr));
  const uint32_t a = r ? r->atom : static_cast<uint32_t>(XCB_ATOM_NONE);
  atoms_[name] = a;
  return a;
}

std::string XcbSource::property(uint32_t win, uint32_t prop, uint32_t type, uint32_t* actualType) {
  // Titles are capped later; 64 KiB is plenty and bounds what a hostile client can make us read.
  Reply r(xcb_get_property_reply(c_, xcb_get_property(c_, 0, win, prop, type, 0, 16384), nullptr));
  if (!r || r->format == 0) return {};
  if (actualType) *actualType = r->type;
  const auto len = static_cast<size_t>(xcb_get_property_value_length(r.p));
  return std::string(static_cast<const char*>(xcb_get_property_value(r.p)), len);
}

uint32_t XcbSource::activeWindow() {
  const auto v = property(root_, atom("_NET_ACTIVE_WINDOW"), XCB_ATOM_WINDOW);
  if (v.size() < 4) return 0;
  uint32_t id;
  std::memcpy(&id, v.data(), 4);
  return id;
}

WindowInfo XcbSource::window(uint32_t id) {
  WindowInfo w;
  w.id = id;
  if (!id) return w;
  const auto cls = property(id, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING);
  if (const auto nul = cls.find('\0'); nul != std::string::npos) {
    w.wmInstance = latin1ToUtf8(cls.substr(0, nul));
    auto rest = cls.substr(nul + 1);
    if (const auto n2 = rest.find('\0'); n2 != std::string::npos) rest.resize(n2);
    w.wmClass = latin1ToUtf8(rest);
  }
  w.title = property(id, atom("_NET_WM_NAME"), atom("UTF8_STRING"));
  if (w.title.empty()) {
    uint32_t type = 0;
    const auto t = property(id, XCB_ATOM_WM_NAME, XCB_GET_PROPERTY_TYPE_ANY, &type);
    w.title = type == XCB_ATOM_STRING ? latin1ToUtf8(t) : t;
  }
  if (const auto pid = property(id, atom("_NET_WM_PID"), XCB_ATOM_CARDINAL); pid.size() >= 4) {
    uint32_t p;
    std::memcpy(&p, pid.data(), 4);
    if (p > 0) w.pid = static_cast<int>(p);
  }
  if (!w.pid && haveRes_) {
    xcb_res_client_id_spec_t spec{id, XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID};
    Reply r(xcb_res_query_client_ids_reply(c_, xcb_res_query_client_ids(c_, 1, &spec), nullptr));
    if (r) {
      for (auto it = xcb_res_query_client_ids_ids_iterator(r.p); it.rem; xcb_res_client_id_value_next(&it)) {
        if ((it.data->spec.mask & XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID) && it.data->length == 4) {
          w.pid = static_cast<int>(*xcb_res_client_id_value_value(it.data));
          break;
        }
      }
    }
  }
  const auto state = property(id, atom("_NET_WM_STATE"), XCB_ATOM_ATOM);
  const uint32_t fs = atom("_NET_WM_STATE_FULLSCREEN");
  for (size_t i = 0; i + 4 <= state.size(); i += 4) {
    uint32_t a;
    std::memcpy(&a, state.data() + i, 4);
    if (a == fs) w.fullscreen = true;
  }
  Reply g(xcb_get_geometry_reply(c_, xcb_get_geometry(c_, id), nullptr));
  if (g) {
    Reply t(xcb_translate_coordinates_reply(c_, xcb_translate_coordinates(c_, id, root_, 0, 0), nullptr));
    if (t) w.geometry = Rect{t->dst_x, t->dst_y, g->width, g->height};
  }
  return w;
}

IdleInfo XcbSource::idle() {
  IdleInfo i;
  if (!haveSaver_) return i;
  Reply r(xcb_screensaver_query_info_reply(c_, xcb_screensaver_query_info(c_, root_), nullptr));
  if (r) {
    i.msSinceInput = r->ms_since_user_input;
    i.screensaverOn = r->state == XCB_SCREENSAVER_STATE_ON;
  }
  return i;
}

std::vector<Monitor> XcbSource::monitors() {
  std::vector<Monitor> out;
  if (haveRandr_) {
    Reply r(xcb_randr_get_monitors_reply(c_, xcb_randr_get_monitors(c_, root_, 1), nullptr));
    if (r) {
      int idx = 0;
      for (auto it = xcb_randr_get_monitors_monitors_iterator(r.p); it.rem && idx < 8; xcb_randr_monitor_info_next(&it)) {
        const auto* m = it.data;
        if (m->width == 0 || m->height == 0) continue;
        out.push_back({idx++, Rect{m->x, m->y, m->width, m->height}, m->primary != 0});
      }
    }
  }
  if (out.empty()) {
    Reply g(xcb_get_geometry_reply(c_, xcb_get_geometry(c_, root_), nullptr));
    if (g) out.push_back({0, Rect{0, 0, g->width, g->height}, true});
  }
  return out;
}

std::vector<int> XcbSource::clientPids() {
  std::vector<int> out;
  const auto list = property(root_, atom("_NET_CLIENT_LIST"), XCB_ATOM_WINDOW);
  for (size_t i = 0; i + 4 <= list.size() && out.size() < 512; i += 4) {
    uint32_t w;
    std::memcpy(&w, list.data() + i, 4);
    if (const auto pid = property(w, atom("_NET_WM_PID"), XCB_ATOM_CARDINAL); pid.size() >= 4) {
      uint32_t p;
      std::memcpy(&p, pid.data(), 4);
      if (p > 0) out.push_back(static_cast<int>(p));
    }
  }
  return out;
}

std::optional<Image> XcbSource::grab(const Rect& r, std::string* err) {
  if (r.w <= 0 || r.h <= 0 || r.w > 16384 || r.h > 16384) {
    if (err) *err = "bad capture rectangle";
    return std::nullopt;
  }
  if (rootDepth_ != 24 && rootDepth_ != 32) {
    if (err) *err = "unsupported root depth " + std::to_string(rootDepth_);
    return std::nullopt;
  }
  Image img;
  img.width = r.w;
  img.height = r.h;
  img.stride = r.w * 4;
  const size_t bytes = static_cast<size_t>(img.stride) * static_cast<size_t>(r.h);

  if (haveShm_) {
    // A private SysV segment, removed as soon as the server has attached it and zeroed before it is detached.
    const int shmid = ::shmget(IPC_PRIVATE, bytes, IPC_CREAT | 0600);
    if (shmid >= 0) {
      void* addr = ::shmat(shmid, nullptr, 0);
      const auto seg = xcb_generate_id(c_);
      bool attached = false;
      if (addr != reinterpret_cast<void*>(-1)) {
        attached = !xcb_request_check(c_, xcb_shm_attach_checked(c_, seg, static_cast<uint32_t>(shmid), 0));
      }
      ::shmctl(shmid, IPC_RMID, nullptr);
      if (attached) {
        Reply rep(xcb_shm_get_image_reply(
            c_,
            xcb_shm_get_image(c_, root_, static_cast<int16_t>(r.x), static_cast<int16_t>(r.y),
                              static_cast<uint16_t>(r.w), static_cast<uint16_t>(r.h), ~0u,
                              XCB_IMAGE_FORMAT_Z_PIXMAP, seg, 0),
            nullptr));
        bool good = rep && rep->size >= bytes;
        if (good) {
          img.px = SecureBytes(bytes);
          std::memcpy(img.px.data(), addr, bytes);
        }
        explicit_bzero(addr, bytes);
        xcb_shm_detach(c_, seg);
        xcb_flush(c_);
        ::shmdt(addr);
        if (good) return img;
      } else if (addr != reinterpret_cast<void*>(-1)) {
        ::shmdt(addr);
      }
    }
    haveShm_ = false;  // remote display or SHM refused: use GetImage from now on
  }

  Reply rep(xcb_get_image_reply(
      c_,
      xcb_get_image(c_, XCB_IMAGE_FORMAT_Z_PIXMAP, root_, static_cast<int16_t>(r.x), static_cast<int16_t>(r.y),
                    static_cast<uint16_t>(r.w), static_cast<uint16_t>(r.h), ~0u),
      nullptr));
  if (!rep) {
    if (err) *err = "GetImage failed";
    return std::nullopt;
  }
  const auto len = static_cast<size_t>(xcb_get_image_data_length(rep.p));
  auto* data = xcb_get_image_data(rep.p);
  if (len < bytes) {
    explicit_bzero(data, len);
    if (err) *err = "GetImage returned a short image";
    return std::nullopt;
  }
  img.px = SecureBytes(bytes);
  std::memcpy(img.px.data(), data, bytes);
  explicit_bzero(data, len);
  return img;
}

XcbSource::Events XcbSource::drain() {
  Events e;
  const uint32_t aActive = atom("_NET_ACTIVE_WINDOW"), aName = atom("_NET_WM_NAME"), aState = atom("_NET_WM_STATE");
  while (auto* ev = xcb_poll_for_event(c_)) {
    if ((ev->response_type & 0x7f) == XCB_PROPERTY_NOTIFY) {
      const auto* p = reinterpret_cast<xcb_property_notify_event_t*>(ev);
      if (p->window == root_ && p->atom == aActive) {
        e.activeChanged = true;
      } else if (p->window == watched_ && (p->atom == aName || p->atom == XCB_ATOM_WM_NAME || p->atom == aState ||
                                           p->atom == XCB_ATOM_WM_CLASS)) {
        e.windowChanged = true;
      }
    }
    std::free(ev);
  }
  e.lost = xcb_connection_has_error(c_) != 0;
  return e;
}

void XcbSource::watch(uint32_t id) {
  if (id == watched_) return;
  const uint32_t none = 0, mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
  // Only our own event selection on that window changes; the other clients' are untouched. Errors (the window is
  // already gone) arrive as events and are ignored.
  if (watched_) xcb_change_window_attributes(c_, watched_, XCB_CW_EVENT_MASK, &none);
  if (id) xcb_change_window_attributes(c_, id, XCB_CW_EVENT_MASK, &mask);
  watched_ = id;
  xcb_flush(c_);
}

}  // namespace thelife::collector
