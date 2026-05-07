#!/usr/bin/env python3
"""
Syringe Pump Pro — Flet Dashboard
Install: pip install flet websocket-client
Run:     python main.py
"""

import flet as ft
import json, threading, time
import websocket  # pip install websocket-client
import re

# ── Constants ──────────────────────────────────────────────
WS_PORT         = 81
DEFAULT_IP      = "192.168.4.1"   # ESP32 AP fixed IP — never changes
RECONNECT_DELAY = 4.0
VOL_MIN, VOL_MAX   = 0.1, 10.0
RATE_MIN, RATE_MAX = 0.01, 35.0   # matches ESP32 MAX_FLOW_RATE
OCCLUSION_THRESH   = 750          # matches ESP32 OCCLUSION_THRESHOLD

# ── Colours ────────────────────────────────────────────────
BG       = "#0A0F1E"
SURFACE  = "#111827"
SURF2    = "#1A2236"
BORDER   = "#1E2D4A"
C_BLUE   = "#2563EB"; C_BLUE_L = "#3B82F6"
C_GREEN  = "#16A34A"; C_GREEN_L= "#22C55E"
C_RED    = "#DC2626"; C_RED_L  = "#EF4444"
C_AMBER  = "#D97706"; C_AMB_L  = "#F59E0B"
C_GRAY   = "#374151"; C_GRAY_L = "#6B7280"
C_TEXT   = "#F0F4FF"; C_MUTED  = "#94A3B8"; C_DIM = "#475569"

def mk_border(width=1, color=BORDER):
    side = ft.BorderSide(width, color)
    return ft.Border(top=side, right=side, bottom=side, left=side)

def mk_card(child, pad=20):
    return ft.Container(
        content=child, bgcolor=SURFACE,
        border=mk_border(1, BORDER), border_radius=16,
        padding=pad,
        margin=ft.Margin(left=12, top=0, right=12, bottom=0),
    )

def section_label(txt):
    return ft.Text(txt, size=9, weight=ft.FontWeight.W_700,
                   color=C_DIM,
                   style=ft.TextStyle(letter_spacing=2))

def mk_btn(label, color, handler, expand=True, height=56, fsize=16):
    btn = ft.FilledButton(
        content=ft.Text(label, size=fsize, weight=ft.FontWeight.W_700, color=C_TEXT),
        height=height,
        style=ft.ButtonStyle(
            bgcolor=color,
            shape=ft.RoundedRectangleBorder(radius=10),
            elevation=4,
            overlay_color="#FFFFFF18",
        ),
        on_click=handler,
    )
    if expand:
        btn.expand = True
    return btn


class PumpApp:
    def __init__(self, page: ft.Page):
        self.page = page
        self.ws   = None
        self._alive = True
        self._lock  = threading.Lock()
        self.esp_ip = DEFAULT_IP
        self.conn_state = "disconnected"
        self.reconnect_count = 0
        self._blink_on = True
        self._first_connect = True        # for Auto-Recovery detection
        self._last_saved_vol = -1.0       # for Black Box indicator
        self._stop_ws   = threading.Event()  # signals current WS to close immediately
        self.tel = dict(
            deliveredVol=0.0, setRate=5.0, setVolume=5.0,
            running=False, occlusion=False, empty=False,
            doseCompleted=False, fsrRaw=0,
            actualRate=0.0, minsRemaining=-1.0,
        )
        self.alerted_state = {"occlusion": False, "empty": False, "doseCompleted": False}
        self._build()
        threading.Thread(target=self._ws_loop,    daemon=True).start()
        threading.Thread(target=self._blink_loop, daemon=True).start()

    # ── Build all controls ─────────────────────────────────
    def _build(self):
        p = self.page
        p.title = "Syringe Pump Pro"
        p.bgcolor = BG
        p.padding = 0
        p.window_width  = 430
        p.window_height = 900

        # Connection badge
        self.dot  = ft.Container(width=9, height=9, border_radius=9, bgcolor=C_RED_L)
        self.ctxt = ft.Text("Offline", size=12, weight=ft.FontWeight.W_600, color=C_RED_L)
        badge = ft.Container(
            content=ft.Row([self.dot, self.ctxt], spacing=6, tight=True),
            bgcolor="#7F1D1D30", border_radius=999,
            border=mk_border(1, "#EF444450"),
            padding=ft.Padding(left=12, top=6, right=12, bottom=6),
        )

        # Black Box save indicator
        self._bb_indicator = ft.Text("", size=11, color=C_GREEN_L,
                                     weight=ft.FontWeight.W_600)

        # IP config
        self.ip_field = ft.TextField(
            value=self.esp_ip, hint_text="e.g. 192.168.43.105",
            dense=True, border_color=BORDER, focused_border_color="#818CF8",
            color=C_TEXT, bgcolor=SURF2, border_radius=10,
            hint_style=ft.TextStyle(color=C_DIM),
            text_style=ft.TextStyle(size=14, weight=ft.FontWeight.W_600),
            content_padding=ft.Padding(left=12, top=10, right=12, bottom=10),
            expand=True,
        )
        self.ip_hint = ft.Text(
            "📶 Connect your PC to WiFi: SyringePump  |  Pass: 12345678", size=11, color=C_DIM)
        btn_conn = ft.FilledButton(
            content=ft.Text("Connect", size=13, weight=ft.FontWeight.W_700, color=C_TEXT),
            style=ft.ButtonStyle(
                bgcolor="#6366F1",
                shape=ft.RoundedRectangleBorder(radius=10),
                padding=ft.Padding(left=16, top=10, right=16, bottom=10),
            ),
            on_click=self._handle_connect,
        )

        # Volume display
        self.t_del  = ft.Text("0.000", size=50, weight=ft.FontWeight.W_700,
                               color=C_BLUE_L,
                               style=ft.TextStyle(font_family="monospace"))
        self.t_tot  = ft.Text("5.000", size=32, weight=ft.FontWeight.W_700,
                               color=C_MUTED,
                               style=ft.TextStyle(font_family="monospace"))
        self.t_unit = ft.Text("mL", size=16, color=C_DIM)
        self.prog   = ft.ProgressBar(value=0.0, color=C_BLUE_L, bgcolor=SURF2,
                                     border_radius=10, height=10)

        # Status banner
        self.s_icon = ft.Text("⏸", size=16)
        self.s_text = ft.Text("System Standby", size=13,
                               weight=ft.FontWeight.W_700, color=C_GRAY_L)
        # Silence button (created here so banner can reference it)
        self.btn_silence = mk_btn("🔕  SILENCE ALARM", C_AMBER, self._handle_silence,
                                expand=True, height=44, fsize=13)
        self.btn_silence.visible = False
        self.banner = ft.Container(
            content=ft.Column([
                ft.Row([self.s_icon, self.s_text], spacing=8,
                       alignment=ft.MainAxisAlignment.CENTER),
                self.btn_silence,
            ], spacing=8),
            bgcolor="#37415140", border_radius=10,
            border=mk_border(1, C_GRAY),
            padding=ft.Padding(left=16, top=10, right=16, bottom=10),
        )

        # Metrics
        self.t_rate = ft.Text("0.0", size=26, weight=ft.FontWeight.W_700,
                               color=C_TEXT,
                               style=ft.TextStyle(font_family="monospace"))
        self.t_mins = ft.Text("--",  size=26, weight=ft.FontWeight.W_700,
                               color=C_TEXT,
                               style=ft.TextStyle(font_family="monospace"))
        self.fsr_bar = ft.ProgressBar(value=0.0, color=C_GREEN_L, bgcolor=BORDER,
                                      border_radius=4, height=6)
        self.fsr_val = ft.Text("0", size=12, color=C_MUTED,
                               style=ft.TextStyle(font_family="monospace"))

        # Settings inputs
        tf_kw = dict(
            dense=True, border_color=BORDER, focused_border_color=C_BLUE_L,
            color=C_TEXT, bgcolor=SURF2, border_radius=10,
            hint_style=ft.TextStyle(color=C_DIM),
            text_style=ft.TextStyle(size=22, weight=ft.FontWeight.W_700,
                                    font_family="monospace"),
            content_padding=ft.Padding(left=16, top=12, right=16, bottom=12),
            keyboard_type=ft.KeyboardType.NUMBER,
        )
        self.tf_vol  = ft.TextField(value="5.0", hint_text="0.1–10.0",  **tf_kw)
        self.tf_rate = ft.TextField(value="5.0", hint_text="0.01–35.0", **tf_kw)

        # Buttons
        self.btn_apply = mk_btn("⚙  APPLY SETTINGS", C_BLUE,  self._handle_apply,
                                expand=True, height=48, fsize=13)
        self.btn_start = mk_btn("▶  START",  C_GREEN, self._handle_start,
                                expand=True, height=60, fsize=18)
        self.btn_pause = mk_btn("⏸  PAUSE",  C_AMBER, self._handle_pause,
                                expand=True, height=60, fsize=18)
        self.btn_reset = mk_btn("↺  RESET DOSE", C_GRAY, self._handle_reset,
                                expand=True, height=48, fsize=13)
        self.btn_jog_rev = mk_btn("⏪  JOG REV", C_GRAY_L, self._handle_jog_rev,
                                expand=True, height=48, fsize=12)
        self.btn_jog_fwd = mk_btn("⏩  JOG FWD", C_GRAY_L, self._handle_jog_fwd,
                                expand=True, height=48, fsize=12)
        # btn_silence already created above (before banner)
        self._action_btns = [self.btn_apply, self.btn_start,
                             self.btn_pause, self.btn_reset,
                             self.btn_jog_rev, self.btn_jog_fwd]

        def _mtile(label, val_ctrl, unit):
            return ft.Container(
                content=ft.Column([
                    ft.Text(label, size=9, weight=ft.FontWeight.W_700,
                            color=C_DIM,
                            style=ft.TextStyle(letter_spacing=1.8)),
                    val_ctrl,
                    ft.Text(unit, size=10, color=C_DIM),
                ], spacing=4, horizontal_alignment=ft.CrossAxisAlignment.CENTER),
                bgcolor=SURF2, border=mk_border(1, BORDER),
                border_radius=10, padding=14, expand=True,
            )

        def _lbl_row(label, hint):
            return ft.Row([
                ft.Text(label, size=11, weight=ft.FontWeight.W_600, color=C_MUTED),
                ft.Text(hint,  size=10, color=C_DIM),
            ], alignment=ft.MainAxisAlignment.SPACE_BETWEEN)

        # ── Layout ────────────────────────────────────────
        header = ft.Container(
            content=ft.Row([
                ft.Column([
                    ft.Text("Syringe Pump Pro", size=20,
                            weight=ft.FontWeight.W_800, color=C_BLUE_L),
                    ft.Text("MEDICAL INFUSION CONTROL", size=9,
                            weight=ft.FontWeight.W_700, color=C_DIM,
                            style=ft.TextStyle(letter_spacing=2)),
                ], spacing=2, tight=True),
                ft.Column([badge, self._bb_indicator],
                          spacing=4, horizontal_alignment=ft.CrossAxisAlignment.END),
            ], alignment=ft.MainAxisAlignment.SPACE_BETWEEN),
            bgcolor=SURFACE,
            border=ft.Border(bottom=ft.BorderSide(1, BORDER)),
            padding=ft.Padding(left=20, top=14, right=20, bottom=14),
        )

        ip_card = mk_card(ft.Column([
            section_label("ESP32 CONNECTION"),
            ft.Row([
                ft.Text("ws://", size=11, color=C_DIM,
                        style=ft.TextStyle(font_family="monospace", weight=ft.FontWeight.W_600)),
                self.ip_field,
                ft.Text(f":{WS_PORT}", size=11, color=C_DIM,
                        style=ft.TextStyle(font_family="monospace", weight=ft.FontWeight.W_600)),
                btn_conn,
            ], spacing=6, vertical_alignment=ft.CrossAxisAlignment.CENTER),
            self.ip_hint,
        ], spacing=10, tight=True))

        tele_card = mk_card(ft.Column([
            section_label("LIVE TELEMETRY"),
            ft.Container(
                content=ft.Row(
                    [self.t_del,
                     ft.Text(" / ", size=32, color=C_DIM),
                     self.t_tot, self.t_unit],
                    alignment=ft.MainAxisAlignment.CENTER,
                    vertical_alignment=ft.CrossAxisAlignment.END,
                ),
                alignment=ft.Alignment(0, 0),
            ),
            ft.Text("DELIVERED  /  TARGET", size=10, color=C_DIM,
                    text_align=ft.TextAlign.CENTER),
            self.prog,
            self.banner,
            ft.Row([_mtile("ACTUAL RATE", self.t_rate, "mL / min"),
                    _mtile("TIME REMAINING", self.t_mins, "minutes")], spacing=10),
            ft.Container(
                content=ft.Row([
                    ft.Text("PRESSURE", size=9, weight=ft.FontWeight.W_700,
                            color=C_DIM, width=62,
                            style=ft.TextStyle(letter_spacing=1.5)),
                    ft.Column([self.fsr_bar], expand=True),
                    self.fsr_val,
                ], spacing=10, vertical_alignment=ft.CrossAxisAlignment.CENTER),
                bgcolor=SURF2, border=mk_border(1, BORDER),
                border_radius=10,
                padding=ft.Padding(left=14, top=10, right=14, bottom=10),
            ),
        ], spacing=12))

        set_card = mk_card(ft.Column([
            section_label("INFUSION SETTINGS"),
            _lbl_row("Target Volume", "0.1 – 10.0 mL"),
            self.tf_vol,
            _lbl_row("Flow Rate", "0.01 – 35.0 mL/min"),
            self.tf_rate,
            self.btn_apply,
        ], spacing=10))

        ctrl_card = mk_card(ft.Column([
            section_label("PUMP CONTROL"),
            ft.Row([self.btn_start, self.btn_pause], spacing=10),
            ft.Row([self.btn_jog_rev, self.btn_jog_fwd], spacing=10),
            self.btn_reset,
        ], spacing=10))

        footer = ft.Container(
            content=ft.Text("Syringe Pump Pro · ESP32 WebSocket · v1.0",
                            size=10, color=C_DIM, text_align=ft.TextAlign.CENTER),
            padding=ft.Padding(left=0, top=0, right=0, bottom=16),
            alignment=ft.Alignment(0, 0),
        )

        p.add(ft.Column([
            header,
            ft.Container(
                content=ft.Column(
                    [ip_card, tele_card, set_card, ctrl_card, footer],
                    spacing=12,
                    scroll=ft.ScrollMode.AUTO,
                ),
                expand=True,
                padding=ft.Padding(left=0, top=12, right=0, bottom=0),
            ),
        ], spacing=0, expand=True))

        self._refresh_ui()


    def _safe_refresh(self):
        """Thread-safe UI refresh — swallows update errors from background threads."""
        try:
            self._refresh_ui()
        except Exception as ex:
            print(f"[UI] refresh error: {ex}")

    # ── WebSocket ──────────────────────────────────────────
    def _ws_loop(self):
        """Persistent reconnection loop."""
        while self._alive:
            self._stop_ws.clear()
            self.conn_state = "connecting"
            self._safe_refresh()

            url = f"ws://{self.esp_ip}:{WS_PORT}"
            try:
                ws = websocket.WebSocketApp(
                    url,
                    on_open=self._on_open,
                    on_message=self._on_message,
                    on_close=self._on_close,
                    on_error=self._on_error,
                )
                with self._lock:
                    self.ws = ws
                ws.run_forever()  # No ping — ESP32 WebSocketsServer doesn't support client-initiated pings
            except Exception as ex:
                print(f"[WS] loop error: {ex}")

            with self._lock:
                self.ws = None

            if not self._alive:
                break

            # Skip countdown if Connect button was pressed
            if self._stop_ws.is_set():
                continue

            self.conn_state = "disconnected"
            self._safe_refresh()
            for i in range(int(RECONNECT_DELAY), 0, -1):
                if not self._alive or self._stop_ws.is_set():
                    break
                self.ip_hint.value = f"⚠️ No connection — retrying in {i}s…"
                self.ip_hint.color = C_AMB_L
                try:
                    self.page.update()
                except Exception:
                    pass
                time.sleep(1)
            self.reconnect_count += 1



    def _on_open(self, ws):
        try:
            self.conn_state = "connected"
            self.ip_hint.value  = f"✅ Connected to ws://{self.esp_ip}:{WS_PORT}"
            self.ip_hint.color  = C_GREEN_L
            self.reconnect_count = 0
            self._safe_refresh()
        except Exception as ex:
            print(f"[WS] _on_open UI error: {ex}")

        # Auto-Recovery: show dialog on reconnect if saved dose > 0
        if self._first_connect:
            self._first_connect = False
            def _check_recovery():
                time.sleep(1.2)  # wait for first telemetry packet
                dv = self.tel.get("deliveredVol", 0.0)
                sv = self.tel.get("setVolume", 5.0)
                sr = self.tel.get("setRate", 5.0)
                if dv > 0.01 and not self.tel.get("running", False):
                    self._confirm(
                        "🔄 Saved Dose Detected",
                        f"The ESP32 has restored a previous session:\n\n"
                        f"  • Delivered : {dv:.3f} mL\n"
                        f"  • Target    : {sv:.3f} mL\n"
                        f"  • Rate      : {sr:.2f} mL/min\n\n"
                        "Do you want to RESUME the saved session or RESET the dose?",
                        yes_text="Resume",
                        no_text="Reset Dose",
                        on_yes=lambda: self._apply_resume_values(sv, sr),
                        on_no=self._reset_fields_and_send,
                    )
            threading.Thread(target=_check_recovery, daemon=True).start()

    def _apply_resume_values(self, sv: float, sr: float):
        """Auto-fill input fields when the user resumes a saved session."""
        self.tf_vol.value = f"{sv:.3f}".rstrip('0').rstrip('.')
        self.tf_rate.value = f"{sr:.2f}".rstrip('0').rstrip('.')
        try:
            self.page.update()
        except Exception:
            pass

    def _reset_fields_and_send(self):
        """Reset input fields to defaults and send reset command."""
        self.tf_vol.value = "5.0"
        self.tf_rate.value = "5.0"
        try:
            self.page.update()
        except Exception:
            pass
        self._send({"cmd": "reset"})



    def _on_message(self, ws, msg):
        try:
            data = json.loads(msg)

            # --- Thread-safe Alarm Popup Trigger ---
            for key, title, msg_text, color in [
                ("occlusion",     "🚨 OCCLUSION DETECTED",
                 "High pressure or blockage detected in the IV line.\nThe motor has been halted.",
                 C_RED_L),
                ("empty",         "⚠️ SYRINGE EMPTY",
                 "The syringe has reached the empty threshold.\nThe motor has been halted.",
                 C_AMB_L),
                ("doseCompleted", "✅ DOSE COMPLETED",
                 "The target volume has been fully delivered.\nThe motor has been halted.",
                 C_GREEN_L),
            ]:
                if data.get(key) and not self.alerted_state[key]:
                    self.alerted_state[key] = True
                    # Capture variables for the closure
                    _title, _msg, _color = title, msg_text, color
                    # Dispatch to UI thread immediately so dialog shows without waiting for a button press
                    self.page.run_task(self._show_alarm_async, _title, _msg, _color)
                elif not data.get(key):
                    self.alerted_state[key] = False

            # Latch alarm flags: keep them shown until ESP confirms clear
            if self.tel.get("occlusion") and not data.get("occlusion"):
                data["occlusion"] = True
            if self.tel.get("empty") and not data.get("empty"):
                data["empty"] = True

            self.tel.update(data)
            self._refresh_ui()

            # Black Box indicator
            if self.tel.get("running") and self.tel.get("deliveredVol", 0) > 0:
                new_vol = round(self.tel["deliveredVol"], 2)
                if new_vol != self._last_saved_vol and int(time.time()) % 10 == 0:
                    self._last_saved_vol = new_vol
                    threading.Thread(target=self._show_save_indicator, daemon=True).start()
        except Exception as ex:
            print(f"[WS] _on_message error: {ex}")

    async def _show_alarm_async(self, title: str, msg: str, color=None):
        """Show an alarm dialog safely from the UI (main) thread."""
        self._alert(title, msg, color=color or C_TEXT)

    def _on_close(self, ws, code, msg):
        try:
            if self.conn_state == "connected":
                self.ip_hint.value = f"⚠️ Lost connection — check WiFi."
                self.ip_hint.color = C_AMB_L
            self.conn_state = "disconnected"
            self._first_connect = True  # reset so auto-recovery triggers on next connect
            self.tel["running"] = False
            self._safe_refresh()
        except Exception as ex:
            print(f"[WS] _on_close error: {ex}")

    def _on_error(self, ws, err):
        print(f"[WS] error: {err}")

    def _show_save_indicator(self):
        """Flash the 💾 Saved indicator for 3 seconds."""
        self._bb_indicator.value = "💾 Auto-saved"
        try: self.page.update()
        except: return
        time.sleep(3)
        self._bb_indicator.value = ""
        try: self.page.update()
        except: pass

    def _send(self, payload: dict) -> bool:
        with self._lock:
            ws = self.ws
        if ws and self.conn_state == "connected":
            try:
                ws.send(json.dumps(payload))
                return True
            except Exception as ex:
                print(f"[WS] send failed: {ex}")
        self._alert("Not Connected",
                    "Cannot send command — not connected to the ESP32.")
        return False

    # ── Blink loop ─────────────────────────────────────────
    def _blink_loop(self):
        while self._alive:
            t = self.tel
            if t["occlusion"]:
                color = C_RED_L if self._blink_on else "#7F1D1D"
                self.s_text.color = color
                self.s_icon.color = color
                self.page.update()
            elif t["empty"]:
                color = C_AMB_L if self._blink_on else "#78350F"
                self.s_text.color = color
                self.s_icon.color = color
                self.page.update()
            self._blink_on = not self._blink_on
            time.sleep(0.55)

    # ── UI Refresh ─────────────────────────────────────────
    def _refresh_ui(self):
        t   = self.tel
        cs  = self.conn_state
        ok  = cs == "connected"

        # Badge
        if cs == "connected":
            bc, tc, bg, brd = C_GREEN_L, C_GREEN_L, "#14532D30", "#22C55E50"
            txt = "Connected"
        elif cs == "connecting":
            bc, tc, bg, brd = C_AMB_L, C_AMB_L, "#78350F30", "#F59E0B50"
            txt = "Connecting"
        else:
            bc, tc, bg, brd = C_RED_L, C_RED_L, "#7F1D1D30", "#EF444450"
            txt = "Offline"
        self.dot.bgcolor  = bc
        self.ctxt.value   = txt
        self.ctxt.color   = tc

        # Volumes
        dv = t["deliveredVol"]; sv = t["setVolume"]
        self.t_del.value  = f"{dv:.3f}"
        self.t_tot.value  = f"{sv:.3f}"
        self.prog.value   = min(1.0, dv / sv) if sv > 0 else 0.0

        # Status banner
        if not ok:
            cls = C_GRAY_L; icon = "🔌"; txt2 = "Disconnected"; bg2 = "#37415140"; brd2 = C_GRAY
        elif t["occlusion"]:
            cls = C_RED_L;  icon = "🚨"; txt2 = "ALARM: OCCLUSION DETECTED"; bg2 = "#7F1D1D30"; brd2 = C_RED_L
        elif t["empty"]:
            cls = C_AMB_L;  icon = "⚠️"; txt2 = "ALARM: SYRINGE EMPTY";      bg2 = "#78350F30"; brd2 = C_AMB_L
        elif t["doseCompleted"]:
            cls = C_GREEN_L; icon = "✅"; txt2 = "DOSE COMPLETED";            bg2 = "#14532D30"; brd2 = C_GREEN_L
        elif t["running"]:
            cls = C_BLUE_L;  icon = "💧"; txt2 = "Infusing…";                 bg2 = "#1E3A8A30"; brd2 = C_BLUE_L
        else:
            cls = C_GRAY_L;  icon = "⏸"; txt2 = "System Standby";            bg2 = "#37415140"; brd2 = C_GRAY

        # Show/hide silence button during alarms
        is_alarming = t["occlusion"] or t["empty"]
        self.btn_silence.visible = ok and is_alarming

        # Only reset colour if not blinking
        if not t["occlusion"] and not t["empty"]:
            self.s_text.color = cls
            self.s_icon.color = cls
        self.s_icon.value   = icon
        self.s_text.value   = txt2
        self.banner.bgcolor = bg2
        self.banner.border  = mk_border(1, brd2)

        # Metrics
        ar = t["actualRate"]; mr = t["minsRemaining"]
        self.t_rate.value = f"{ar:.1f}"
        self.t_mins.value = "--" if mr < 0 else f"{mr:.1f}"
        if ok and t["running"]:
            self.t_rate.color = C_BLUE_L
            self.t_mins.color = C_BLUE_L
        else:
            self.t_rate.color = C_TEXT
            self.t_mins.color = C_TEXT

        # FSR bar
        fsr = t["fsrRaw"]
        self.fsr_bar.value = min(1.0, fsr / 4095)
        self.fsr_bar.color = (C_RED_L if fsr >= OCCLUSION_THRESH
                              else C_AMB_L if fsr >= OCCLUSION_THRESH * 0.7
                              else C_GREEN_L)
        self.fsr_val.value = str(fsr)

        # Button states (User requested: ALL buttons clickable at ANY time)
        for b in self._action_btns:
            b.disabled = not ok

        try:
            self.page.update()
        except Exception:
            pass

    # ── Validation ─────────────────────────────────────────
    def _validate(self):
        errors = []
        try:
            vol = float(self.tf_vol.value)
            if not (VOL_MIN <= vol <= VOL_MAX):
                raise ValueError()
        except ValueError:
            vol = None
            errors.append(f"• Volume: {VOL_MIN}–{VOL_MAX} mL")
            self.tf_vol.border_color = C_RED_L
        else:
            self.tf_vol.border_color = BORDER

        try:
            rate = float(self.tf_rate.value)
            if not (RATE_MIN <= rate <= RATE_MAX):
                raise ValueError()
        except ValueError:
            rate = None
            errors.append(f"• Rate: {RATE_MIN}–{RATE_MAX} mL/min")
            self.tf_rate.border_color = C_RED_L
        else:
            self.tf_rate.border_color = BORDER

        if errors:
            self.page.update()
            self._alert(
                "⚠️ Invalid Input",
                "Fix the following fields:\n\n" + "\n".join(errors) +
                "\n\nNo command was sent to the pump.",
            )
            return None
        return vol, rate

    # ── Dialogs ────────────────────────────────────────────
    def _alert(self, title: str, msg: str, color=C_TEXT):
        def close(e):
            dlg.open = False
            self.page.update()
        dlg = ft.AlertDialog(
            modal=True,
            title=ft.Text(title, weight=ft.FontWeight.W_700, color=color),
            content=ft.Text(msg, color=C_MUTED),
            actions=[ft.TextButton(content=ft.Text("OK"), on_click=close,
                                   style=ft.ButtonStyle(color=C_BLUE_L))],
            bgcolor=SURF2,
            shape=ft.RoundedRectangleBorder(radius=16),
        )
        self.page.overlay.append(dlg)
        self.page.update()
        dlg.open = True
        self.page.update()

    def _confirm(self, title: str, msg: str, on_yes, yes_text="Reset", no_text="Cancel", on_no=None):
        def yes(e):
            dlg.open = False
            self.page.update()
            on_yes()
        def no(e):
            dlg.open = False
            self.page.update()
            if on_no: on_no()
        dlg = ft.AlertDialog(
            modal=True,
            title=ft.Text(title, weight=ft.FontWeight.W_700, color=C_TEXT),
            content=ft.Text(msg, color=C_MUTED),
            actions=[
                ft.TextButton(content=ft.Text(no_text), on_click=no,
                              style=ft.ButtonStyle(color=C_GRAY_L)),
                ft.TextButton(content=ft.Text(yes_text), on_click=yes,
                              style=ft.ButtonStyle(color=C_RED_L)),
            ],
            bgcolor=SURF2,
            shape=ft.RoundedRectangleBorder(radius=16),
        )
        self.page.overlay.append(dlg)
        self.page.update()
        dlg.open = True
        self.page.update()

    # ── Command Handlers ───────────────────────────────────
    def _handle_connect(self, e):
        raw = (self.ip_field.value or "").strip()
        raw = raw.replace("ws://", "").split(":")[0]
        if not raw or not re.match(r'^[\d\.a-zA-Z-]+$', raw):
            self.ip_field.border_color = C_RED_L
            self.ip_hint.value = "❌ Enter a valid IP (e.g. 192.168.43.105)"
            self.ip_hint.color = C_RED_L
            self.page.update()
            return
        self.ip_field.border_color = BORDER
        self.esp_ip = raw
        self.ip_hint.value = f"🔄 Connecting to ws://{raw}:{WS_PORT}…"
        self.ip_hint.color = C_DIM
        self.page.update()

        # Signal the reconnect loop to skip countdown and retry immediately
        self._stop_ws.set()
        with self._lock:
            if self.ws:
                try:
                    self.ws.close()
                except Exception:
                    pass

    def _handle_apply(self, e):
        vals = self._validate()
        if vals is None: return
        vol, rate = vals
        if self._send({"cmd": "settings", "rate": rate, "volume": vol}):
            self._alert("✅ Settings Applied",
                        f"Target: {vol} mL @ {rate} mL/min sent to pump.")

    def _handle_start(self, e):
        vals = self._validate()
        if vals is None: return
        vol, rate = vals
        # Clear alarm latch before starting
        self.tel["occlusion"] = False
        self.tel["empty"] = False
        self.tel["doseCompleted"] = False
        self._send({"cmd": "settings", "rate": rate, "volume": vol})
        time.sleep(0.05)
        self._send({"cmd": "start"})

    def _handle_pause(self, e):
        # Optimistic UI update
        self.tel["running"] = False
        self._refresh_ui()
        self._send({"cmd": "pause"})

    def _handle_reset(self, e):
        self._confirm(
            "↺ Confirm Reset",
            "Are you sure you want to reset the current dose?\n\n"
            "Delivered volume will be cleared and the pump will stop.",
            on_yes=self._reset_fields_and_send,
        )

    def _handle_jog_fwd(self, e):
        self._send({"cmd": "jog", "dir": 1})

    def _handle_jog_rev(self, e):
        self._send({"cmd": "jog", "dir": -1})

    def _handle_silence(self, e):
        self._send({"cmd": "silence"})
        # Clear local latch so UI banner goes back to Standby
        self.tel["occlusion"] = False
        self.tel["empty"]     = False
        self._refresh_ui()

def main(page: ft.Page):
    PumpApp(page)

ft.run(main)
