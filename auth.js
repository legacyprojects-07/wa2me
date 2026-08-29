'use strict';

const fs = require('fs');
const path = require('path');
const { useMultiFileAuthState, BufferJSON } = require('@whiskeysockets/baileys');

/**
 * Hybrid auth state: filesystem + PostgreSQL backup.
 *
 * - Uses filesystem for fast read/write (Baileys' native format)
 * - Backs up to PostgreSQL on every creds update
 * - On boot, restores from PostgreSQL if filesystem is empty (after redeploy)
 */

async function useHybridAuthState(authDir, pool) {
  // Ensure auth directory exists
  if (!fs.existsSync(authDir)) fs.mkdirSync(authDir, { recursive: true });

  // Check if filesystem has auth
  const credsFile = path.join(authDir, 'creds.json');
  const hasFilesystemAuth = fs.existsSync(credsFile);

  if (!hasFilesystemAuth) {
    console.log('[Auth] No filesystem auth — checking PostgreSQL...');
    const restored = await restoreFromPostgres(authDir, pool);
    if (restored) {
      console.log('[Auth] Restored auth from PostgreSQL');
    } else {
      console.log('[Auth] No saved auth — new session required (scan QR)');
    }
  } else {
    console.log('[Auth] Loaded auth from filesystem');
  }

  // Use Baileys' native filesystem auth
  const { state, saveCreds: originalSaveCreds } = await useMultiFileAuthState(authDir);

  // Wrap saveCreds to also backup to PostgreSQL
  const saveCreds = async () => {
    await originalSaveCreds();
    await backupToPostgres(authDir, pool);
  };

  return { state, saveCreds };
}

/**
 * Backup entire auth state from filesystem to PostgreSQL.
 */
async function backupToPostgres(authDir, pool) {
  try {
    await ensureTable(pool);

    // Backup creds.json
    const credsFile = path.join(authDir, 'creds.json');
    if (fs.existsSync(credsFile)) {
      const credsData = JSON.parse(fs.readFileSync(credsFile, 'utf-8'));
      await pool.query(
        `INSERT INTO wa_auth (key, value, updated_at)
         VALUES ('creds', $1::jsonb, NOW())
         ON CONFLICT (key) DO UPDATE SET value = $1::jsonb, updated_at = NOW()`,
        [JSON.stringify(credsData)]
      );
    }

    // Backup keys/ directory
    const keysDir = path.join(authDir, 'keys');
    if (fs.existsSync(keysDir)) {
      const files = fs.readdirSync(keysDir).filter(f => f.endsWith('.json'));
      for (const file of files) {
        const key = file.replace('.json', '');
        const data = JSON.parse(fs.readFileSync(path.join(keysDir, file), 'utf-8'));
        await pool.query(
          `INSERT INTO wa_auth (key, value, updated_at)
           VALUES ($1, $2::jsonb, NOW())
           ON CONFLICT (key) DO UPDATE SET value = $2::jsonb, updated_at = NOW()`,
          [key, JSON.stringify(data)]
        );
      }
    }
  } catch (err) {
    console.error('[Auth] Backup failed:', err.message);
  }
}

/**
 * Restore auth state from PostgreSQL to filesystem.
 */
async function restoreFromPostgres(authDir, pool) {
  try {
    // Check if table exists
    const { rows: tableCheck } = await pool.query(`
      SELECT EXISTS (
        SELECT FROM information_schema.tables WHERE table_name = 'wa_auth'
      ) AS exists
    `);
    if (!tableCheck[0]?.exists) return false;

    const { rows } = await pool.query('SELECT key, value FROM wa_auth');
    if (rows.length === 0) return false;

    // Ensure directories
    const keysDir = path.join(authDir, 'keys');
    if (!fs.existsSync(keysDir)) fs.mkdirSync(keysDir, { recursive: true });

    for (const row of rows) {
      if (row.key === 'creds') {
        fs.writeFileSync(
          path.join(authDir, 'creds.json'),
          JSON.stringify(row.value, BufferJSON.replacer)
        );
      } else {
        fs.writeFileSync(
          path.join(keysDir, `${row.key}.json`),
          JSON.stringify(row.value, BufferJSON.replacer)
        );
      }
    }

    return true;
  } catch (err) {
    console.error('[Auth] Restore failed:', err.message);
    return false;
  }
}

async function ensureTable(pool) {
  await pool.query(`
    CREATE TABLE IF NOT EXISTS wa_auth (
      key        TEXT PRIMARY KEY,
      value      JSONB NOT NULL,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
    )
  `);
}

async function getAuthStats(pool) {
  try {
    const { rows } = await pool.query(
      "SELECT COUNT(*)::int AS total, MAX(updated_at) AS last_updated FROM wa_auth"
    );
    return { totalKeys: rows[0]?.total || 0, lastUpdated: rows[0]?.last_updated || null };
  } catch {
    return { totalKeys: 0, lastUpdated: null };
  }
}

async function clearAuth(pool) {
  await pool.query('DELETE FROM wa_auth');
  console.log('[Auth] Cleared all auth data');
}

module.exports = { useHybridAuthState, getAuthStats, clearAuth };
