// Vector generator for the Qt-free core tests.
//
// Requires the real Baileys package (with its deps) and Node >= 18:
//   BAILEYS_DIR=/path/to/node_modules/@whiskeysockets/baileys \
//     node qt/tests/gen_proto.mjs.mjs
//
// The checked-in *_vectors.txt files were generated this way against
// @whiskeysockets/baileys 6.7.22, so the C++ port is verified against the real
// reference implementation rather than hand-copied constants.
import crypto from 'crypto';
import fs from 'fs';

import { pathToFileURL } from 'url';

const BAILEYS_DIR = process.env.BAILEYS_DIR;
if (!BAILEYS_DIR) {
  console.error('set BAILEYS_DIR to the @whiskeysockets/baileys package directory');
  process.exit(1);
}
const baileysImport = p => pathToFileURL(BAILEYS_DIR + p).href;

const { proto } = await import(baileysImport('/WAProto/index.js'));

const lines = [];

// md5 vectors
for (const s of ['', 'abc', '2.3000.1035939187', 'a'.repeat(200)]) {
  const buf = Buffer.from(s, 'utf8');
  lines.push(`MD5 ${buf.toString('hex') || '-'} ${crypto.createHash('md5').update(buf).digest('hex')}`);
}

// HandshakeMessage.clientHello
const eph = crypto.randomBytes(32);
const hello = proto.HandshakeMessage.encode(proto.HandshakeMessage.fromObject({
  clientHello: { ephemeral: eph },
})).finish();
lines.push(`HELLO ${eph.toString('hex')} ${hello.toString('hex')}`);

// ServerHello (encode so the C++ decoder can be tested too)
const serverHelloObj = {
  ephemeral: crypto.randomBytes(32),
  static: crypto.randomBytes(32),
  payload: crypto.randomBytes(48),
};
const serverHello = proto.HandshakeMessage.encode(proto.HandshakeMessage.fromObject({
  serverHello: serverHelloObj,
})).finish();
lines.push(`SERVERHELLO ${serverHello.toString('hex')} ${serverHelloObj.ephemeral.toString('hex')} ${serverHelloObj.static.toString('hex')} ${serverHelloObj.payload.toString('hex')}`);

// clientFinish
const finish = proto.HandshakeMessage.encode(proto.HandshakeMessage.fromObject({
  clientFinish: { static: crypto.randomBytes(32), payload: crypto.randomBytes(64) },
})).finish();
lines.push(`FINISH ${finish.toString('hex')}`);

// DeviceProps as Baileys builds it for registration
const appVersionBuf = crypto.createHash('md5').update('2.3000.1035939187').digest();
const companion = {
  os: 'Ubuntu',
  platformType: 15,
  requireFullSync: true,
  historySyncConfig: {
    storageQuotaMb: 10240,
    inlineInitialPayloadInE2EeMsg: true,
    supportCallLogHistory: false,
    supportBotUserAgentChatHistory: true,
    supportCagReactionsAndPolls: true,
    supportBizHostedMsg: true,
    supportRecentSyncChunkMessageCountTuning: true,
    supportHostedGroupMsg: true,
    supportFbidBotChatHistory: true,
    supportMessageAssociation: true,
  },
};
const deviceProps = proto.DeviceProps.encode(proto.DeviceProps.fromObject(companion)).finish();
lines.push(`DEVICEPROPS ${deviceProps.toString('hex')}`);

// Full registration ClientPayload, exactly as generateRegistrationNode builds it
const registrationId = 1234567890;
const signedPreKey = { keyId: 1, keyPair: { public: crypto.randomBytes(32), private: crypto.randomBytes(32) }, signature: crypto.randomBytes(64) };
const signedIdentityKey = { public: crypto.randomBytes(32), private: crypto.randomBytes(32) };

const encodeBigEndian = (n, len) => {
  if (!len) {
    const bytes = [];
    let v = n;
    while (v > 0) { bytes.unshift(v & 0xff); v >>= 8; }
    return Buffer.from(bytes.length ? bytes : [0]);
  }
  const out = Buffer.alloc(len);
  for (let i = len - 1; i >= 0; i--) { out[i] = n & 0xff; n >>= 8; }
  return out;
};

const userAgent = {
  appVersion: { primary: 2, secondary: 3000, tertiary: 1035939187 },
  platform: 1,            // WEB
  releaseChannel: 0,      // RELEASE
  osVersion: '0.1',
  device: 'Desktop',
  osBuildNumber: '0.1',
  localeLanguageIso6391: 'en',
  mnc: '000',
  mcc: '000',
  localeCountryIso31661Alpha2: 'US',
};
const registerPayload = {
  connectType: 1,        // WIFI_UNKNOWN
  connectReason: 1,      // USER_ACTIVATED
  userAgent,
  webInfo: { webSubPlatform: 0 },
  passive: false,
  pull: false,
  devicePairingData: {
    buildHash: appVersionBuf,
    deviceProps,
    eRegid: encodeBigEndian(registrationId),
    eKeytype: Buffer.from([5]),
    eIdent: signedIdentityKey.public,
    eSkeyId: encodeBigEndian(signedPreKey.keyId, 3),
    eSkeyVal: signedPreKey.keyPair.public,
    eSkeySig: signedPreKey.signature,
  },
};
const clientPayload = proto.ClientPayload.encode(proto.ClientPayload.fromObject(registerPayload)).finish();
lines.push(`REGPAYLOAD 1234567890 1 1035939187 ${signedIdentityKey.public.toString('hex')} ${signedPreKey.keyId} ${signedPreKey.keyPair.public.toString('hex')} ${signedPreKey.signature.toString('hex')} ${deviceProps.toString('hex')} ${clientPayload.toString('hex')}`);

// Login payload (existing session)
const loginPayload = {
  connectType: 1,
  connectReason: 1,
  userAgent,
  webInfo: { webSubPlatform: 0 },
  passive: false,
  pull: true,
  username: 919999999999,
  device: 0,
};
const loginEncoded = proto.ClientPayload.encode(proto.ClientPayload.fromObject(loginPayload)).finish();
lines.push(`LOGINPAYLOAD 919999999999 0 1 1035939187 ${loginEncoded.toString('hex')}`);

fs.writeFileSync('/home/user/wa2me/qt/tests/proto_vectors.txt', lines.join('\n') + '\n');
console.log('wrote', lines.length);
