"""Flask demo UI (prelim_spec.md §8): a pure API client for exercising the backend. Pages for resources are
generated from the contract; the only hand-written pages are login, the calendar, settings and a few
resource-specific panels (task occurrences, detection history)."""
from __future__ import annotations

import json
import os
import re
import secrets
from datetime import date, datetime, timedelta
from functools import wraps
from pathlib import Path
from urllib.parse import quote, urlsplit
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

import requests
from flask import Flask, abort, flash, g, redirect, render_template, request, url_for

from .api import Api, ApiError
from .contract import Contract, Resource
from .forms import REFERENCES, Field, FormError, build_fields, keep_submitted, parse_value, to_body
from .sessions import SessionStore

REPO = Path(__file__).resolve().parents[2]
UUID = re.compile(r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")
SESSION_COOKIE = "tl_demo"
LOGIN_CSRF_COOKIE = "tl_demo_login"
LIST_COLUMNS = ("status", "kind", "area", "starts_at", "started_at", "due_at", "due_on", "period_start",
                "last_seen_at", "revoked_at")


def create_app(config: dict | None = None) -> Flask:
    app = Flask(__name__)
    app.config.update(
        API_URL=os.environ.get("THELIFE_API_URL", "http://127.0.0.1:8080"),
        CONTRACT=os.environ.get("THELIFE_CONTRACT", str(REPO / "api/generated/openapi.yaml")),
        STAGE=int(os.environ.get("THELIFE_STAGE", "1")),
        SECURE_COOKIES=os.environ.get("DEMO_SECURE_COOKIES", "0") == "1",
        HTTP_SESSION=None,  # tests inject a requests.Session with a fake transport
    )
    app.config.update(config or {})
    app.secret_key = secrets.token_bytes(32)  # flash messages only; nothing sensitive lives in Flask's cookie
    app.config["SESSION_COOKIE_HTTPONLY"] = True
    app.config["SESSION_COOKIE_SAMESITE"] = "Strict"
    app.config["SESSION_COOKIE_SECURE"] = app.config["SECURE_COOKIES"]

    contract = Contract(Path(app.config["CONTRACT"]), app.config["STAGE"])
    store = SessionStore()
    http = app.config["HTTP_SESSION"] or requests.Session()
    app.extensions["demo"] = {"contract": contract, "store": store}

    def api(token: str | None = None) -> Api:
        return Api(app.config["API_URL"], token, session=http)

    # ------------------------------------------------------------------ request plumbing
    @app.before_request
    def load_session():
        g.sid = request.cookies.get(SESSION_COOKIE)
        g.s = store.get(g.sid)
        g.api = api(g.s.token if g.s else None)
        g.tz = user_tz(g.s.user if g.s else {})
        g.refs = {}
        if request.method == "POST":
            sent = request.form.get("csrf", "")
            # Signed in: the per-session token. The login form: a double-submit cookie.
            expected = request.cookies.get(LOGIN_CSRF_COOKIE, "") if request.endpoint == "login" or not g.s \
                else g.s.csrf
            if not expected or not secrets.compare_digest(sent, expected):
                abort(400, "Form expired or forged (CSRF check failed). Reload the page and try again.")

    @app.after_request
    def headers(resp):
        resp.headers["Content-Security-Policy"] = (
            "default-src 'none'; style-src 'self'; img-src 'self'; form-action 'self'; "
            "frame-ancestors 'none'; base-uri 'none'")
        resp.headers["X-Content-Type-Options"] = "nosniff"
        resp.headers["Referrer-Policy"] = "no-referrer"
        resp.headers["Cache-Control"] = "no-store"
        return resp

    @app.errorhandler(ApiError)
    def api_error(e: ApiError):
        if e.status == 401:
            store.drop(g.get("sid"))
            flash("Your session ended. Sign in again.", "error")
            return redirect(url_for("login", next=request.full_path if request.method == "GET" else None))
        return render_template("error.html", error=e), e.status if e.status >= 400 else 500

    @app.context_processor
    def globals_():
        return {"contract": contract, "me": g.s.user if g.get("s") else None,
                "csrf": g.s.csrf if g.get("s") else request.cookies.get(LOGIN_CSRF_COOKIE, "")}

    @app.template_filter("local")
    def local(value, fmt="%a %d %b %Y %H:%M"):
        if not value:
            return ""
        try:
            return datetime.fromisoformat(str(value)).astimezone(g.tz).strftime(fmt)
        except ValueError:
            return value

    @app.template_filter("pretty")
    def pretty(value):
        return json.dumps(value, indent=2, ensure_ascii=False)

    def login_required(view):
        @wraps(view)
        def wrapper(*a, **kw):
            if not g.s:
                return redirect(url_for("login", next=request.full_path))
            return view(*a, **kw)
        return wrapper

    # ------------------------------------------------------------------ auth
    @app.route("/login", methods=["GET", "POST"])
    def login():
        nxt = safe_next(request.values.get("next"))
        if request.method == "POST":
            try:
                session = api().post("/api/v1/auth/login", {"email": request.form.get("email", ""),
                                                             "password": request.form.get("password", "")})
            except ApiError as e:
                if e.status == 401:
                    flash("Wrong email or password.", "error")
                elif e.status == 429:
                    flash("Too many attempts. Wait a minute and try again.", "error")
                else:
                    flash(str(e), "error")
                return login_page(nxt, request.form.get("email", "")), e.status
            store.drop(g.sid)
            sid = store.create(session["token"], session["user"])
            resp = redirect(nxt or url_for("home"))
            resp.set_cookie(SESSION_COOKIE, sid, httponly=True, samesite="Strict", secure=app.config["SECURE_COOKIES"])
            resp.delete_cookie(LOGIN_CSRF_COOKIE)
            return resp
        if g.s:
            return redirect(nxt or url_for("home"))
        return login_page(nxt, "")

    def login_page(nxt, email):
        token = request.cookies.get(LOGIN_CSRF_COOKIE) or secrets.token_urlsafe(32)
        resp = app.make_response(render_template("login.html", next=nxt, email=email, login_csrf=token))
        resp.set_cookie(LOGIN_CSRF_COOKIE, token, httponly=True, samesite="Strict", secure=app.config["SECURE_COOKIES"])
        return resp

    @app.post("/logout")
    @login_required
    def logout():
        try:
            g.api.call("POST", "/api/v1/auth/logout")
        except ApiError:
            pass  # the local session goes regardless
        store.drop(g.sid)
        resp = redirect(url_for("login"))
        resp.delete_cookie(SESSION_COOKIE)
        return resp

    # ------------------------------------------------------------------ calendar
    @app.get("/")
    @login_required
    def home():
        return calendar_page(None, 7)

    @app.get("/calendar")
    @login_required
    def calendar():
        try:
            start = date.fromisoformat(request.args["start"]) if request.args.get("start") else None
            days = max(1, min(62, int(request.args.get("days", 7))))
        except ValueError:
            abort(400, "start must be YYYY-MM-DD and days a number")
        return calendar_page(start, days)

    def calendar_page(start: date | None, days: int):
        day_start = int(g.s.user.get("day_start_hour", 0))
        today = (datetime.now(g.tz) - timedelta(hours=day_start)).date()
        start = start or today
        frm = datetime(start.year, start.month, start.day, day_start, tzinfo=g.tz)
        to = frm + timedelta(days=days)
        view = g.api.get("/api/v1/calendar", **{"from": frm.isoformat(), "to": to.isoformat()})
        items = view["items"]
        if view.get("truncated"):
            flash(f"Showing the first {len(items)} entries only; pick fewer days to see the rest.", "error")
        by_day: dict[str, list] = {(start + timedelta(days=i)).isoformat(): [] for i in range(days)}
        for e in items:
            day = e.get("occurs_on") if e["all_day"] and e.get("occurs_on") else (
                datetime.fromisoformat(e["starts_at"]).astimezone(g.tz) - timedelta(hours=day_start)).date().isoformat()
            by_day.setdefault(day, []).append(e)
        for v in by_day.values():
            v.sort(key=lambda e: (not e["all_day"], e["starts_at"]))
        return render_template("calendar.html", by_day=sorted(by_day.items()), start=start, days=days, today=today,
                               prev=(start - timedelta(days=days)).isoformat(),
                               next=(start + timedelta(days=days)).isoformat())

    @app.post("/tasks/<task_id>/occurrences/<day>/<verb>")
    @login_required
    def occurrence_action(task_id, day, verb):
        check_id(task_id)
        try:
            date.fromisoformat(day)
        except ValueError:
            abort(404)
        if verb not in ("complete", "skip"):
            abort(404)
        body = {}
        if verb == "skip" and request.form.get("reason", "").strip():
            body["reason"] = request.form["reason"].strip()
        try:
            g.api.call("POST", f"/api/v1/tasks/{quote(task_id)}/occurrences/{day}:{verb}", body=body)
            flash(f"Occurrence on {day} {'done' if verb == 'complete' else 'skipped'}.", "ok")
        except ApiError as e:
            if e.status == 401:
                raise
            flash(describe(e), "error")
        return redirect(safe_next(request.form.get("next")) or url_for("home"))

    # ------------------------------------------------------------------ generic resources
    def resource(prefix: str) -> Resource:
        r = contract.resources.get(prefix)
        if not r:
            abort(404)
        return r

    def options_for(name: str, s: dict) -> list[tuple[str, str]]:
        if name == "scopes":
            return [(x, x) for x in contract.api_key_scopes()]
        target = REFERENCES.get(name)
        if not target or target not in contract.resources or s.get("format", "uuid") != "uuid":
            return []
        if target not in g.refs:
            r = contract.resources[target]
            items = g.api.get(r.collection, limit=200)["items"]
            g.refs[target] = [(i["id"], r.title_of(i)) for i in items]
        return g.refs[target]

    def fields_for(schema: dict, values: dict | None = None) -> list[Field]:
        return build_fields(schema, contract.deref, g.tz, values=values, options_for=options_for)

    @app.get("/r/<prefix>")
    @login_required
    def resource_list(prefix):
        r = resource(prefix)
        filters = build_fields({"properties": {p["name"]: p["schema"] for p in r.filters}}, contract.deref, g.tz,
                               options_for=options_for, required_override=False)
        for f in filters:
            if f.kind == "checkbox":  # tri-state in a query: any / true / false
                f.kind, f.options = "select", [("true", "true"), ("false", "false")]
        keep_submitted(filters, request.args)
        params = {k: request.args.get(k) for k in ("cursor", "limit")}
        try:
            for f in filters:
                present, v = parse_value(f, request.args, g.tz)
                if present and v not in ([], ""):
                    params[f.name] = v
        except FormError as e:
            for f in filters:
                f.error = e.errors.get(f.name, "")
        page = g.api.get(r.collection, **params)
        props = r.schema.get("properties", {})
        columns = [c for c in LIST_COLUMNS if c in props][:4]
        return render_template("resource_list.html", r=r, page=page, columns=columns, filters=filters,
                               args=request.args)

    @app.route("/r/<prefix>/new", methods=["GET", "POST"])
    @login_required
    def resource_new(prefix):
        r = resource(prefix)
        if not r.create:
            abort(404)
        fields = fields_for(r.create.body or {})
        if request.method == "POST":
            try:
                created = g.api.post(r.collection, to_body(fields, request.form, g.tz))
            except (FormError, ApiError) as e:
                return form_error(e, fields, "resource_form.html", r=r, op=r.create, item=None)
            if r.create.returns_secret:
                return render_template("token_once.html", r=r, created=created)
            flash(f"Created {r.title_of(created)}.", "ok")
            return redirect(url_for("resource_get", prefix=prefix, id=created["id"]))
        return render_template("resource_form.html", r=r, op=r.create, fields=fields, item=None)

    @app.get("/r/<prefix>/<id>")
    @login_required
    def resource_get(prefix, id):
        r = resource(prefix)
        check_id(id)
        item = g.api.get(r.item.replace("{id}", id))
        fields = fields_for(r.update.body or {}, item) if r.update else []
        actions = [(op, fields_for(op.body or {})) for op in r.actions]
        extra = panels(r, id, item)
        return render_template("resource_item.html", r=r, item=item, fields=fields, actions=actions, **extra)

    def panels(r: Resource, id: str, item: dict) -> dict:
        """Resource-specific read panels."""
        out: dict = {}
        if r.prefix == "tasks" and item.get("rrule") and contract.ops.get("tasks.occurrences"):
            start = request.args.get("from") or None
            out["occurrences"] = g.api.get(f"/api/v1/tasks/{id}/occurrences", **{
                "from": start, "to": request.args.get("to")})["items"]
        if r.prefix == "detections" and contract.ops.get("detections.history"):
            out["history"] = g.api.get(f"/api/v1/detections/{id}/history")
        return out

    @app.post("/r/<prefix>/<id>/edit")
    @login_required
    def resource_edit(prefix, id):
        r = resource(prefix)
        check_id(id)
        if not r.update:
            abort(404)
        item = g.api.get(r.item.replace("{id}", id))
        fields = fields_for(r.update.body or {}, item)
        version = request.form.get("version", "")
        try:
            body = to_body(fields, request.form, g.tz, original=item)
            if not body:
                flash("Nothing changed.", "ok")
                return redirect(url_for("resource_get", prefix=prefix, id=id))
            g.api.call("PATCH", r.item.replace("{id}", id), body=body,
                       if_match=version if r.update.if_match else None)
        except (FormError, ApiError) as e:
            if isinstance(e, ApiError) and e.status == 412:
                flash("Someone changed this item since you opened it. The page now shows the latest version; "
                      "make your edit again.", "error")
                return redirect(url_for("resource_get", prefix=prefix, id=id))
            actions = [(op, fields_for(op.body or {})) for op in r.actions]
            return form_error(e, fields, "resource_item.html", r=r, item=item, actions=actions, **panels(r, id, item))
        flash("Saved.", "ok")
        return redirect(url_for("resource_get", prefix=prefix, id=id))

    @app.post("/r/<prefix>/<id>/delete")
    @login_required
    def resource_delete(prefix, id):
        r = resource(prefix)
        check_id(id)
        if not r.delete:
            abort(404)
        g.api.call("DELETE", r.item.replace("{id}", id),
                   if_match=request.form.get("version") if r.delete.if_match else None)
        flash({"delete": "Deleted.", "revoke": "Revoked."}.get(r.delete.verb, "Done."), "ok")
        return redirect(url_for("resource_list", prefix=prefix))

    @app.post("/r/<prefix>/<id>/do/<verb>")
    @login_required
    def resource_action(prefix, id, verb):
        r = resource(prefix)
        check_id(id)
        op = next((a for a in r.actions if a.verb == verb), None)
        if not op:
            abort(404)
        fields = fields_for(op.body or {})
        try:
            result = g.api.post(op.path.replace("{id}", id), to_body(fields, request.form, g.tz) if op.body else None)
        except (FormError, ApiError) as e:
            if isinstance(e, ApiError) and e.status == 401:
                raise
            flash(f"{op.summary}: {describe(e)}", "error")
            return redirect(url_for("resource_get", prefix=prefix, id=id))
        flash(f"{op.summary}: done.", "ok")
        target = result.get("id", id) if isinstance(result, dict) else id
        return redirect(url_for("resource_get", prefix=prefix, id=target))

    # ------------------------------------------------------------------ settings
    @app.route("/settings", methods=["GET", "POST"])
    @login_required
    def settings():
        me_op = contract.ops.get("auth.update_me")
        me = g.api.get("/api/v1/me")
        g.s.user = me
        fields = fields_for(me_op.body if me_op else {}, me)
        if request.method == "POST" and request.form.get("form") == "profile":
            try:
                body = to_body(fields, request.form, g.tz, original=me)
                if body:
                    g.s.user = g.api.call("PATCH", "/api/v1/me", body=body).body
                flash("Profile saved." if body else "Nothing changed.", "ok")
                return redirect(url_for("settings"))
            except (FormError, ApiError) as e:
                return form_error(e, fields, "settings.html")
        if request.method == "POST" and request.form.get("form") == "password":
            try:
                g.api.call("POST", "/api/v1/me/password", body={
                    "current_password": request.form.get("current_password", ""),
                    "new_password": request.form.get("new_password", "")})
                flash("Password changed. Other sessions were signed out.", "ok")
            except ApiError as e:
                if e.status == 401 and "current_password" not in str(e.errors):
                    raise
                flash(describe(e), "error")
            return redirect(url_for("settings"))
        return render_template("settings.html", fields=fields)

    # ------------------------------------------------------------------ helpers
    def form_error(e, fields, template, **ctx):
        keep_submitted(fields, request.form)
        if isinstance(e, FormError):
            errs, status = e.errors, 422
        else:
            if e.status == 401:
                raise e
            errs, status = e.field_errors(), e.status
            flash(str(e), "error")
        for f in fields:
            f.error = errs.get(f.name, "")
        unplaced = {k: v for k, v in errs.items() if k not in {f.name for f in fields}}
        for k, v in unplaced.items():
            flash(f"{k}: {v}" if k else v, "error")
        return render_template(template, fields=fields, **ctx), status

    return app


def describe(e: Exception) -> str:
    if isinstance(e, FormError):
        return "; ".join(f"{k}: {v}" for k, v in e.errors.items())
    fields = "; ".join(f"{k}: {v}" if k else v for k, v in e.field_errors().items())
    return str(e) + (f" ({fields})" if fields else "")


def user_tz(user: dict) -> ZoneInfo:
    try:
        return ZoneInfo(user.get("timezone") or "UTC")
    except (ZoneInfoNotFoundError, ValueError):
        return ZoneInfo("UTC")


def check_id(value: str) -> None:
    if not UUID.match(value):
        abort(404)


def safe_next(value: str | None) -> str | None:
    """Only same-site relative paths, so ?next= cannot redirect elsewhere."""
    if not value:
        return None
    parts = urlsplit(value)
    if parts.scheme or parts.netloc or not value.startswith("/") or value.startswith("//") or "\\" in value:
        return None
    return value

