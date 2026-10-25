'use strict';

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const multer = require('multer');
const { httpError, sanitizeFileName } = require('./helpers');

function envMb(name, fallback) {
  const n = parseFloat(process.env[name]);
  return Number.isFinite(n) && n > 0 ? n : fallback;
}

const MAX_UPLOAD_MB = envMb('MAX_UPLOAD_MB', 25);
const MAX_UPLOAD_TOTAL_MB = envMb('MAX_UPLOAD_TOTAL_MB', 50);
const MAX_UPLOAD_BYTES = Math.floor(MAX_UPLOAD_MB * 1024 * 1024);
const MAX_UPLOAD_TOTAL_BYTES = Math.floor(MAX_UPLOAD_TOTAL_MB * 1024 * 1024);
const MAX_FILES = Math.min(20, Math.max(1, parseInt(process.env.MAX_UPLOAD_FILES, 10) || 8));

let uploadDir = path.resolve(process.env.UPLOAD_TMP_DIR || './data/tmp');

function ensureDir() {
  fs.mkdirSync(uploadDir, { recursive: true });
}

function init(dir) {
  if (dir) uploadDir = path.resolve(dir);
  ensureDir();
}

/** Remove temp files left behind by a crashed upload. */
function cleanupStaleFiles(maxAgeMs = 60 * 60 * 1000) {
  try {
    if (!fs.existsSync(uploadDir)) return 0;
    const cutoff = Date.now() - maxAgeMs;
    let removed = 0;
    for (const name of fs.readdirSync(uploadDir)) {
      const file = path.join(uploadDir, name);
      try {
        const st = fs.statSync(file);
        if (st.isFile() && st.mtimeMs < cutoff) { fs.unlinkSync(file); removed++; }
      } catch {}
    }
    return removed;
  } catch {
    return 0;
  }
}

const storage = multer.diskStorage({
  destination: (req, file, cb) => {
    try {
      ensureDir();
      cb(null, uploadDir);
    } catch (err) {
      cb(err);
    }
  },
  filename: (req, file, cb) => {
    cb(null, `${Date.now()}-${crypto.randomBytes(8).toString('hex')}`);
  },
});

const upload = multer({
  storage,
  limits: {
    fileSize: MAX_UPLOAD_BYTES,
    files: MAX_FILES,
    fields: 40,
    parts: MAX_FILES + 48,
  },
});

/** Multer middleware; safely ignores non-multipart requests. */
const anyFiles = upload.any();

function cleanupFiles(files) {
  for (const file of files || []) {
    if (!file?.path) continue;
    try { fs.unlinkSync(file.path); } catch {}
  }
}

/**
 * Read all uploaded files into memory, then delete the temporary copies.
 * Baileys needs a Buffer anyway, but disk storage keeps concurrent uploads
 * from ballooning the Render instance's RAM before we are ready to send.
 */
async function readUploadedFiles(files) {
  const list = (files || []).filter(Boolean);
  if (list.length === 0) return [];

  const total = list.reduce((sum, f) => sum + (Number(f.size) || 0), 0);
  if (total > MAX_UPLOAD_TOTAL_BYTES) {
    cleanupFiles(list);
    throw httpError(413, `Total upload is too large (max ${MAX_UPLOAD_TOTAL_MB} MB across all files)`, 'UPLOAD_TOTAL_TOO_LARGE');
  }

  try {
    const out = [];
    for (const file of list) {
      if (!file.size) throw httpError(400, `Uploaded file "${file.originalname || 'file'}" is empty`, 'EMPTY_FILE');
      const buffer = await fs.promises.readFile(file.path);
      out.push({
        buffer,
        mimetype: file.mimetype || 'application/octet-stream',
        fileName: sanitizeFileName(file.originalname || file.fieldname || 'file'),
        size: buffer.length,
        field: file.fieldname,
      });
    }
    return out;
  } finally {
    cleanupFiles(list);
  }
}

/**
 * Decode a JSON base64 upload. Supports data URLs and URL-safe base64.
 * Kept for old J2ME/Qt clients; multipart is strongly preferred.
 */
function decodeBase64(value) {
  let s = String(value || '').trim();
  if (!s) throw httpError(400, 'mediaBase64 is empty', 'EMPTY_BASE64');

  const dataUrl = /^data:([^;,]+)?(?:;[^,]*)?;base64,(.*)$/is.exec(s);
  let declaredMime = null;
  if (dataUrl) {
    declaredMime = dataUrl[1] || null;
    s = dataUrl[2];
  }

  s = s.replace(/\s+/g, '').replace(/-/g, '+').replace(/_/g, '/');
  if (!/^[A-Za-z0-9+/]*={0,2}$/.test(s)) {
    throw httpError(400, 'mediaBase64 is not valid base64', 'INVALID_BASE64');
  }
  const pad = s.length % 4;
  if (pad === 2) s += '==';
  else if (pad === 3) s += '=';
  else if (pad === 1) throw httpError(400, 'mediaBase64 is not valid base64', 'INVALID_BASE64');

  const buffer = Buffer.from(s, 'base64');
  if (!buffer.length) throw httpError(400, 'mediaBase64 decoded to an empty file', 'EMPTY_BASE64');
  if (buffer.length > MAX_UPLOAD_BYTES) {
    throw httpError(413, `Base64 media is too large (max ${MAX_UPLOAD_MB} MB)`, 'FILE_TOO_LARGE');
  }
  return { buffer, declaredMime };
}

module.exports = {
  init,
  cleanupStaleFiles,
  anyFiles,
  readUploadedFiles,
  cleanupFiles,
  decodeBase64,
  MAX_UPLOAD_MB,
  MAX_UPLOAD_TOTAL_MB,
  MAX_UPLOAD_BYTES,
  MAX_UPLOAD_TOTAL_BYTES,
  MAX_FILES,
};
