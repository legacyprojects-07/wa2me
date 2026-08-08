// Persists Baileys' auth creds + signal keys in MongoDB instead of the
// local filesystem, since Render's free tier wipes disk on every restart.
//
// One Mongo document per key, stored in a single collection, keyed by
// a synthetic _id like "creds" or "app-state-sync-key-<id>".

const { initAuthCreds, proto, BufferJSON } = require('@whiskeysockets/baileys');

async function useMongoAuthState(collection) {
  const writeData = async (id, data) => {
    await collection.updateOne(
      { _id: id },
      { $set: { value: JSON.stringify(data, BufferJSON.replacer) } },
      { upsert: true }
    );
  };

  const readData = async (id) => {
    const doc = await collection.findOne({ _id: id });
    if (!doc || !doc.value) return null;
    return JSON.parse(doc.value, BufferJSON.reviver);
  };

  const removeData = async (id) => {
    await collection.deleteOne({ _id: id });
  };

  const creds = (await readData('creds')) || initAuthCreds();

  return {
    state: {
      creds,
      keys: {
        get: async (type, ids) => {
          const data = {};
          await Promise.all(
            ids.map(async (id) => {
              let value = await readData(`${type}-${id}`);
              if (type === 'app-state-sync-key' && value) {
                value = proto.Message.AppStateSyncKeyData.fromObject(value);
              }
              data[id] = value;
            })
          );
          return data;
        },
        set: async (data) => {
          const tasks = [];
          for (const category of Object.keys(data)) {
            for (const id of Object.keys(data[category])) {
              const value = data[category][id];
              const key = `${category}-${id}`;
              tasks.push(value ? writeData(key, value) : removeData(key));
            }
          }
          await Promise.all(tasks);
        },
      },
    },
    saveCreds: () => writeData('creds', creds),
  };
}

module.exports = { useMongoAuthState };
