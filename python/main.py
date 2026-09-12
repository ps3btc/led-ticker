from arduino.app_bricks.web_ui import WebUI
from arduino.app_utils import App, Bridge, Logger

MAX_CHARS = 16  # must match MAX_CHARS in sketch/sketch.ino
DEFAULT_SPEED = 240  # ms per pixel of travel; must match the slider in assets/index.html
MIN_SPEED = 40
MAX_SPEED = 1200

logger = Logger("led-ticker")
ui = WebUI()

state = {"text": "", "playing": False, "speed": DEFAULT_SPEED}


def _coerce_speed(raw):
    if isinstance(raw, bool) or not isinstance(raw, (int, float)):
        return None
    return max(MIN_SPEED, min(MAX_SPEED, int(raw)))


def _sanitize(raw):
    if not isinstance(raw, str):
        return ""
    printable = "".join(ch for ch in raw if " " <= ch <= "~")
    return printable.strip()[:MAX_CHARS]


def _matrix_ready():
    """True/false from the sketch, or None if the board would not answer."""
    try:
        return bool(Bridge.call("matrix_ready"))
    except Exception as exc:
        logger.warning(f"matrix_ready probe failed: {exc}")
        return None


ABSENT = "The board is running but no LED matrix answered on I2C — check the Qwiic cable."


def _reply():
    ready = _matrix_ready()
    if ready is False:
        return {"error": ABSENT, **state}
    return {"ok": True, "matrix": ready, **state}


def submit_text(payload: dict):
    text = _sanitize((payload or {}).get("text"))
    if not text:
        return {"error": f"Enter between 1 and {MAX_CHARS} printable characters"}
    try:
        Bridge.call("set_speed", state["speed"])
        Bridge.call("set_text", text)
    except Exception as exc:
        logger.warning(f"set_text failed: {exc}")
        return {"error": f"The board did not accept the text: {exc}"}
    state.update(text=text, playing=True)
    logger.info(f"scrolling {text!r} at {state['speed']} ms/step")
    return _reply()


def set_speed(payload: dict):
    speed = _coerce_speed((payload or {}).get("speed"))
    if speed is None:
        return {"error": f"Speed must be a number between {MIN_SPEED} and {MAX_SPEED}"}
    try:
        Bridge.call("set_speed", speed)
    except Exception as exc:
        logger.warning(f"set_speed failed: {exc}")
        return {"error": f"The board did not accept the speed: {exc}"}
    state["speed"] = speed
    logger.info(f"speed {speed} ms/step")
    return _reply()


def stop_text():
    try:
        Bridge.call("stop_text")
    except Exception as exc:
        logger.warning(f"stop_text failed: {exc}")
        return {"error": f"The board did not accept the stop: {exc}"}
    state.update(text="", playing=False)
    logger.info("stopped")
    return _reply()


def get_state():
    return {"matrix": _matrix_ready(), **state}


ui.expose_api("POST", "/api/text", submit_text)
ui.expose_api("POST", "/api/speed", set_speed)
ui.expose_api("POST", "/api/stop", stop_text)
ui.expose_api("GET", "/api/state", get_state)

App.run()
