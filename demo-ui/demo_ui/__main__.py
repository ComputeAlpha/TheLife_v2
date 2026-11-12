"""python -m demo_ui  — development server on 127.0.0.1 (DEMO_PORT, default 5000)."""
import os

from . import create_app

if __name__ == "__main__":
    create_app().run(host="127.0.0.1", port=int(os.environ.get("DEMO_PORT", "5000")), debug=False)
