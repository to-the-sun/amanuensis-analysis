import os
import subprocess
import sys

def ensure_extension_built():
    """Checks if the extension is built and builds it if necessary."""
    current_dir = os.path.dirname(os.path.abspath(__file__))
    ext_file = None
    for f in os.listdir(current_dir):
        if f.startswith("cumulative_transience.") and (f.endswith(".so") or f.endswith(".pyd")):
            ext_file = os.path.join(current_dir, f)
            break

    source_pyx = os.path.join(current_dir, "ct_extension.pyx")
    source_c = os.path.join(current_dir, "cumulative_transience.c")

    needs_build = False
    if ext_file is None:
        needs_build = True
    else:
        ext_mtime = os.path.getmtime(ext_file)
        if os.path.exists(source_pyx) and os.path.getmtime(source_pyx) > ext_mtime:
            needs_build = True
        elif os.path.exists(source_c) and os.path.getmtime(source_c) > ext_mtime:
            needs_build = True

    if needs_build:
        print("Notice: Extension module is missing or outdated. Attempting to build...")
        old_cwd = os.getcwd()
        os.chdir(current_dir)
        try:
            # Use the same command as the Makefile
            python_cmd = "python" if os.name == "nt" else "python3"
            subprocess.run([python_cmd, "setup.py", "build_ext", "--inplace"], check=True)
            print("Extension module built successfully.")
        except Exception as e:
            print(f"Warning: Failed to build extension module: {e}")
            print("The script may fail to import 'cumulative_transience'.")
        finally:
            os.chdir(old_cwd)
