import logging
import os
from pathlib import Path

from fastapi import FastAPI, HTTPException
from fastapi.responses import FileResponse, JSONResponse

app = FastAPI(title="Embroidery Manifest Server")
logger = logging.getLogger("uvicorn.error")

ROOT_DIR = Path(os.environ.get("EMBROIDERY_ROOT", "/data"))


@app.get("/manifest.json")
def manifest():
    """Return a JSON array of all files with relative path, size, and mtime."""
    if not ROOT_DIR.is_dir():
        raise HTTPException(status_code=500, detail=f"Root directory not found: {ROOT_DIR}")

    entries = []
    for filepath in sorted(ROOT_DIR.rglob("*")):
        if not filepath.is_file():
            continue
        stat = filepath.stat()
        rel = filepath.relative_to(ROOT_DIR).as_posix()
        entries.append({
            "path": rel,
            "size": stat.st_size,
            "mtime": int(stat.st_mtime),
        })

    logger.info("Manifest requested: %d files", len(entries))
    return JSONResponse(content=entries)


@app.get("/files/{path:path}")
def get_file(path: str):
    """Stream a file for download by relative path."""
    full = ROOT_DIR / path
    # Prevent path traversal
    try:
        full.resolve().relative_to(ROOT_DIR.resolve())
    except ValueError:
        raise HTTPException(status_code=403, detail="Path traversal not allowed")

    if not full.is_file():
        raise HTTPException(status_code=404, detail="File not found")

    logger.info("Serving file: %s (%d bytes)", path, full.stat().st_size)
    return FileResponse(full, filename=full.name)
