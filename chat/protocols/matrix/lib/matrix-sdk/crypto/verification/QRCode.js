"use strict";

Object.defineProperty(exports, "__esModule", {
  value: true
});
exports.SHOW_QR_CODE_METHOD = exports.SCAN_QR_CODE_METHOD = exports.ReciprocateQRCode = exports.QrCodeEvent = exports.QRCodeData = void 0;
var _Base = require("./Base");
var _Error = require("./Error");
var _olmlib = require("../olmlib");
var _logger = require("../../logger");
function _defineProperty(obj, key, value) { key = _toPropertyKey(key); if (key in obj) { Object.defineProperty(obj, key, { value: value, enumerable: true, configurable: true, writable: true }); } else { obj[key] = value; } return obj; }
function _toPropertyKey(arg) { var key = _toPrimitive(arg, "string"); return typeof key === "symbol" ? key : String(key); }
function _toPrimitive(input, hint) { if (typeof input !== "object" || input === null) return input; var prim = input[Symbol.toPrimitive]; if (prim !== undefined) { var res = prim.call(input, hint || "default"); if (typeof res !== "object") return res; throw new TypeError("@@toPrimitive must return a primitive value."); } return (hint === "string" ? String : Number)(input); }
const SHOW_QR_CODE_METHOD = "m.qr_code.show.v1";
exports.SHOW_QR_CODE_METHOD = SHOW_QR_CODE_METHOD;
const SCAN_QR_CODE_METHOD = "m.qr_code.scan.v1";
exports.SCAN_QR_CODE_METHOD = SCAN_QR_CODE_METHOD;
let QrCodeEvent;
exports.QrCodeEvent = QrCodeEvent;
(function (QrCodeEvent) {
  QrCodeEvent["ShowReciprocateQr"] = "show_reciprocate_qr";
})(QrCodeEvent || (exports.QrCodeEvent = QrCodeEvent = {}));
class ReciprocateQRCode extends _Base.VerificationBase {
  constructor(...args) {
    super(...args);
    _defineProperty(this, "reciprocateQREvent", void 0);
    _defineProperty(this, "doVerification", async () => {
      if (!this.startEvent) {
        // TODO: Support scanning QR codes
        throw new Error("It is not currently possible to start verification" + "with this method yet.");
      }
      const {
        qrCodeData
      } = this.request;
      // 1. check the secret
      if (this.startEvent.getContent()["secret"] !== qrCodeData?.encodedSharedSecret) {
        throw (0, _Error.newKeyMismatchError)();
      }

      // 2. ask if other user shows shield as well
      await new Promise((resolve, reject) => {
        this.reciprocateQREvent = {
          confirm: resolve,
          cancel: () => reject((0, _Error.newUserCancelledError)())
        };
        this.emit(QrCodeEvent.ShowReciprocateQr, this.reciprocateQREvent);
      });

      // 3. determine key to sign / mark as trusted
      const keys = {};
      switch (qrCodeData?.mode) {
        case Mode.VerifyOtherUser:
          {
            // add master key to keys to be signed, only if we're not doing self-verification
            const masterKey = qrCodeData.otherUserMasterKey;
            keys[`ed25519:${masterKey}`] = masterKey;
            break;
          }
        case Mode.VerifySelfTrusted:
          {
            const deviceId = this.request.targetDevice.deviceId;
            keys[`ed25519:${deviceId}`] = qrCodeData.otherDeviceKey;
            break;
          }
        case Mode.VerifySelfUntrusted:
          {
            const masterKey = qrCodeData.myMasterKey;
            keys[`ed25519:${masterKey}`] = masterKey;
            break;
          }
      }

      // 4. sign the key (or mark own MSK as verified in case of MODE_VERIFY_SELF_TRUSTED)
      await this.verifyKeys(this.userId, keys, (keyId, device, keyInfo) => {
        // make sure the device has the expected keys
        const targetKey = keys[keyId];
        if (!targetKey) throw (0, _Error.newKeyMismatchError)();
        if (keyInfo !== targetKey) {
          _logger.logger.error("key ID from key info does not match");
          throw (0, _Error.newKeyMismatchError)();
        }
        for (const deviceKeyId in device.keys) {
          if (!deviceKeyId.startsWith("ed25519")) continue;
          const deviceTargetKey = keys[deviceKeyId];
          if (!deviceTargetKey) throw (0, _Error.newKeyMismatchError)();
          if (device.keys[deviceKeyId] !== deviceTargetKey) {
            _logger.logger.error("master key does not match");
            throw (0, _Error.newKeyMismatchError)();
          }
        }
      });
    });
  }
  static factory(channel, baseApis, userId, deviceId, startEvent, request) {
    return new ReciprocateQRCode(channel, baseApis, userId, deviceId, startEvent, request);
  }

  // eslint-disable-next-line @typescript-eslint/naming-convention
  static get NAME() {
    return "m.reciprocate.v1";
  }
}
exports.ReciprocateQRCode = ReciprocateQRCode;
const CODE_VERSION = 0x02; // the version of binary QR codes we support
const BINARY_PREFIX = "MATRIX"; // ASCII, used to prefix the binary format
var Mode; // We do not trust the master key
(function (Mode) {
  Mode[Mode["VerifyOtherUser"] = 0] = "VerifyOtherUser";
  Mode[Mode["VerifySelfTrusted"] = 1] = "VerifySelfTrusted";
  Mode[Mode["VerifySelfUntrusted"] = 2] = "VerifySelfUntrusted";
})(Mode || (Mode = {}));
class QRCodeData {
  constructor(mode, sharedSecret,
  // only set when mode is MODE_VERIFY_OTHER_USER, master key of other party at time of generating QR code
  otherUserMasterKey,
  // only set when mode is MODE_VERIFY_SELF_TRUSTED, device key of other party at time of generating QR code
  otherDeviceKey,
  // only set when mode is MODE_VERIFY_SELF_UNTRUSTED, own master key at time of generating QR code
  myMasterKey, buffer) {
    this.mode = mode;
    this.sharedSecret = sharedSecret;
    this.otherUserMasterKey = otherUserMasterKey;
    this.otherDeviceKey = otherDeviceKey;
    this.myMasterKey = myMasterKey;
    this.buffer = buffer;
  }
  static async create(request, client) {
    const sharedSecret = QRCodeData.generateSharedSecret();
    const mode = QRCodeData.determineMode(request, client);
    let otherUserMasterKey = null;
    let otherDeviceKey = null;
    let myMasterKey = null;
    if (mode === Mode.VerifyOtherUser) {
      const otherUserCrossSigningInfo = client.getStoredCrossSigningForUser(request.otherUserId);
      otherUserMasterKey = otherUserCrossSigningInfo.getId("master");
    } else if (mode === Mode.VerifySelfTrusted) {
      otherDeviceKey = await QRCodeData.getOtherDeviceKey(request, client);
    } else if (mode === Mode.VerifySelfUntrusted) {
      const myUserId = client.getUserId();
      const myCrossSigningInfo = client.getStoredCrossSigningForUser(myUserId);
      myMasterKey = myCrossSigningInfo.getId("master");
    }
    const qrData = QRCodeData.generateQrData(request, client, mode, sharedSecret, otherUserMasterKey, otherDeviceKey, myMasterKey);
    const buffer = QRCodeData.generateBuffer(qrData);
    return new QRCodeData(mode, sharedSecret, otherUserMasterKey, otherDeviceKey, myMasterKey, buffer);
  }

  /**
   * The unpadded base64 encoded shared secret.
   */
  get encodedSharedSecret() {
    return this.sharedSecret;
  }
  getBuffer() {
    return this.buffer;
  }
  static generateSharedSecret() {
    const secretBytes = new Uint8Array(11);
    global.crypto.getRandomValues(secretBytes);
    return (0, _olmlib.encodeUnpaddedBase64)(secretBytes);
  }
  static async getOtherDeviceKey(request, client) {
    const myUserId = client.getUserId();
    const otherDevice = request.targetDevice;
    const device = otherDevice.deviceId ? client.getStoredDevice(myUserId, otherDevice.deviceId) : undefined;
    if (!device) {
      throw new Error("could not find device " + otherDevice?.deviceId);
    }
    return device.getFingerprint();
  }
  static determineMode(request, client) {
    const myUserId = client.getUserId();
    const otherUserId = request.otherUserId;
    let mode = Mode.VerifyOtherUser;
    if (myUserId === otherUserId) {
      // Mode changes depending on whether or not we trust the master cross signing key
      const myTrust = client.checkUserTrust(myUserId);
      if (myTrust.isCrossSigningVerified()) {
        mode = Mode.VerifySelfTrusted;
      } else {
        mode = Mode.VerifySelfUntrusted;
      }
    }
    return mode;
  }
  static generateQrData(request, client, mode, encodedSharedSecret, otherUserMasterKey, otherDeviceKey, myMasterKey) {
    const myUserId = client.getUserId();
    const transactionId = request.channel.transactionId;
    const qrData = {
      prefix: BINARY_PREFIX,
      version: CODE_VERSION,
      mode,
      transactionId,
      firstKeyB64: "",
      // worked out shortly
      secondKeyB64: "",
      // worked out shortly
      secretB64: encodedSharedSecret
    };
    const myCrossSigningInfo = client.getStoredCrossSigningForUser(myUserId);
    if (mode === Mode.VerifyOtherUser) {
      // First key is our master cross signing key
      qrData.firstKeyB64 = myCrossSigningInfo.getId("master");
      // Second key is the other user's master cross signing key
      qrData.secondKeyB64 = otherUserMasterKey;
    } else if (mode === Mode.VerifySelfTrusted) {
      // First key is our master cross signing key
      qrData.firstKeyB64 = myCrossSigningInfo.getId("master");
      qrData.secondKeyB64 = otherDeviceKey;
    } else if (mode === Mode.VerifySelfUntrusted) {
      // First key is our device's key
      qrData.firstKeyB64 = client.getDeviceEd25519Key();
      // Second key is what we think our master cross signing key is
      qrData.secondKeyB64 = myMasterKey;
    }
    return qrData;
  }
  static generateBuffer(qrData) {
    let buf = Buffer.alloc(0); // we'll concat our way through life

    const appendByte = b => {
      const tmpBuf = Buffer.from([b]);
      buf = Buffer.concat([buf, tmpBuf]);
    };
    const appendInt = i => {
      const tmpBuf = Buffer.alloc(2);
      tmpBuf.writeInt16BE(i, 0);
      buf = Buffer.concat([buf, tmpBuf]);
    };
    const appendStr = (s, enc, withLengthPrefix = true) => {
      const tmpBuf = Buffer.from(s, enc);
      if (withLengthPrefix) appendInt(tmpBuf.byteLength);
      buf = Buffer.concat([buf, tmpBuf]);
    };
    const appendEncBase64 = b64 => {
      const b = (0, _olmlib.decodeBase64)(b64);
      const tmpBuf = Buffer.from(b);
      buf = Buffer.concat([buf, tmpBuf]);
    };

    // Actually build the buffer for the QR code
    appendStr(qrData.prefix, "ascii", false);
    appendByte(qrData.version);
    appendByte(qrData.mode);
    appendStr(qrData.transactionId, "utf-8");
    appendEncBase64(qrData.firstKeyB64);
    appendEncBase64(qrData.secondKeyB64);
    appendEncBase64(qrData.secretB64);
    return buf;
  }
}
exports.QRCodeData = QRCodeData;