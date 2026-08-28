'use strict';

/**
 * PostgreSQL-backed WhatsApp auth state for Baileys.
 *
 * Stores credentials and Signal protocol keys in a `wa_auth` table
 * so the WhatsApp session survives server restarts and redeploys.
 */

let initAuthCreds, BufferJSON;
try {
  ({ initAuthCreds, BufferJSON } = require('@whiskeysockets/baileys'));
} catch (err) {
  console.error('[Auth] Failed to import Baileys auth utilities:', err.message);
}

/**
 * Create a PostgreSQL-backed auth state.
 * @param {import('pg').Pool} pool - PostgreSQL connection pool
 * @returns {Promise<{state: {creds, keys}, saveCreds: Function}>}
 */
async function usePostgresAuthState(pool) {
  // Ensure table exists
  await pool.query(`
    CREATE TABLE IF NOT EXISTS wa_auth (
      key       TEXT PRIMARY KEY,
      value     JSONB NOT NULL,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
    )
  `);

  // ── Load or initialize credentials ──────────────────────────────────────

  let creds;
  try {
    const { rows } = await pool.query("SELECT value FROM wa_auth WHERE key = 'creds'");
    if (rows.length > 0) {
      // Deserialize: JSONB → JSON string → parse with BufferJSON.reviver → restore Buffers
      creds = JSON.parse(JSON.stringify(rows[0].value), BufferJSON.reviver);
      console.log('[Auth] Restored credentials from PostgreSQL');
    } else {
      creds = initAuthCreds();
      console.log('[Auth] No saved credentials — new session required');
    }
  } catch (err) {
    console.error('[Auth] Failed to load creds:', err.message);
    creds = initAuthCreds();
  }

  // ── Save credentials ────────────────────────────────────────────────────

  const saveCreds = async () => {
    try {
      // Serialize: creds → JSON with BufferJSON.replacer → store as JSONB
      const serialized = JSON.parse(JSON.stringify(creds, BufferJSON.replacer));
      await pool.query(
        `INSERT INTO wa_auth (key, value, updated_at)
         VALUES ('creds', $1::jsonb, NOW())
         ON CONFLICT (key) DO UPDATE SET value = $1::jsonb, updated_at = NOW()`,
        [JSON.stringify(serialized)]
      );
    } catch (err) {
      console.error('[Auth] Failed to save creds:', err.message);
    }
  };

  // ── Signal protocol keys store ──────────────────────────────────────────

  const keys = {
    /**
     * Get keys by type and IDs.
     * @param {string} type - Key type (session, pre-key, sender-key, app-state-sync-key, etc.)
     * @param {string[]} ids - Array of key IDs
     * @returns {Promise<Object>} Map of id → key data
     */
    get: async (type, ids) => {
      const data = {};
      if (!ids || ids.length === 0) return data;

      const dbKeys = ids.map(id => `${type}:${id}`);

      try {
        const { rows } = await pool.query(
          'SELECT key, value FROM wa_auth WHERE key = ANY($1::text[])',
          [dbKeys]
        );

        for (const row of rows) {
          // Extract the ID part after the type prefix
          const id = row.key.substring(type.length + 1);
          data[id] = JSON.parse(JSON.stringify(row.value), BufferJSON.reviver);
        }
      } catch (err) {
        console.error(`[Auth] Failed to get keys (type=${type}):`, err.message);
      }

      return data;
    },

    /**
     * Set multiple keys at once.
     * @param {Object} data - Map of "type:id" → key data
     */
    set: async (data) => {
      const entries = Object.entries(data);
      if (entries.length === 0) return;

      const client = await pool.connect();
      try {
        await client.query('BEGIN');

        for (const [key, value] of entries) {
          const serialized = JSON.parse(JSON.stringify(value, BufferJSON.replacer));
          await client.query(
            `INSERT INTO wa_auth (key, value, updated_at)
             VALUES ($1, $2::jsonb, NOW())
             ON CONFLICT (key) DO UPDATE SET value = $2::jsonb, updated_at = NOW()`,
            [key, JSON.stringify(serialized)]
          );
        }

        await client.query('COMMIT');
      } catch (err) {
        await client.query('ROLLBACK');
        console.error('[Auth] Failed to set keys:', err.message);
      } finally {
        client.release();
      }
    },
  };

  return {
    state: { creds, keys },
    saveCreds,
  };
}

/**
 * Get auth stats (how many keys are stored).
 */
async function getAuthStats(pool) {
  try {
    const { rows } = await pool.query(
      "SELECT COUNT(*)::int AS total, MAX(updated_at) AS last_updated FROM wa_auth"
    );
    return {
      totalKeys: rows[0]?.total || 0,
      lastUpdated: rows[0]?.last_updated || null,
    };
  } catch {
    return { totalKeys: 0, lastUpdated: null };
  }
}

/**
 * Clear all auth data (force re-link).
 */
async function clearAuth(pool) {
  await pool.query('DELETE FROM wa_auth');
  console.log('[Auth] Cleared all auth data from PostgreSQL');
}

module.exports = { usePostgresAuthState, getAuthStats, clearAuth };
