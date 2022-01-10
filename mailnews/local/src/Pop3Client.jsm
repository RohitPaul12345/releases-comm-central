/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const EXPORTED_SYMBOLS = ["Pop3Client"];

var { setTimeout } = ChromeUtils.import("resource://gre/modules/Timer.jsm");
var { AppConstants } = ChromeUtils.import(
  "resource://gre/modules/AppConstants.jsm"
);
var { CommonUtils } = ChromeUtils.import("resource://services-common/utils.js");
var { Services } = ChromeUtils.import("resource://gre/modules/Services.jsm");
var { MailCryptoUtils } = ChromeUtils.import(
  "resource:///modules/MailCryptoUtils.jsm"
);
var { Pop3Authenticator } = ChromeUtils.import(
  "resource:///modules/MailAuthenticator.jsm"
);

/**
 * A structure to represent a response received from the server. A response can
 * be a single status line of a multi-line data block.
 * @typedef {Object} Pop3Response
 * @property {boolean} success - True for a positive status indicator ("+OK","+").
 * @property {string} status - The status indicator, can be "+OK", "-ERR" or "+".
 * @property {string} statusText - The status line of the response excluding the
 *   status indicator.
 * @property {string} data - The part of a multi-line data block excluding the
 *   status line.
 */

const POP3_AUTH_MECH_UNDEFINED = 0x200;

/**
 * A class to interact with POP3 server.
 */
class Pop3Client {
  /**
   * @param {nsIPop3IncomingServer} server - The associated server instance.
   */
  constructor(server) {
    this._server = server.QueryInterface(Ci.nsIMsgIncomingServer);
    this._authenticator = new Pop3Authenticator(server);

    // Somehow, Services.io.newURI("pop3://localhost") doesn't work, what we
    // need is just a valid nsIMsgMailNewsUrl to propagate OnStopRunningUrl and
    // secInfo.
    this.runningUri = Services.io
      .newURI(`smtp://${this._server.realHostName}:${this._server.port}`)
      .mutate()
      .setScheme("pop3")
      .finalize()
      .QueryInterface(Ci.nsIMsgMailNewsUrl);

    // A list of auth methods detected from the EHLO response.
    this._supportedAuthMethods = [];
    // A list of auth methods that worth a try.
    this._possibleAuthMethods = [];
    // Auth method set by user preference.
    this._preferredAuthMethods =
      {
        [Ci.nsMsgAuthMethod.passwordCleartext]: ["PLAIN", "LOGIN"],
        [Ci.nsMsgAuthMethod.passwordEncrypted]: ["CRAM-MD5"],
        [Ci.nsMsgAuthMethod.GSSAPI]: ["GSSAPI"],
        [Ci.nsMsgAuthMethod.NTLM]: ["NTLM"],
        [Ci.nsMsgAuthMethod.OAuth2]: ["XOAUTH2"],
        [Ci.nsMsgAuthMethod.secure]: ["CRAM-MD5", "GSSAPI"],
      }[server.authMethod] || [];
    // The next auth method to try if the current failed.
    this._nextAuthMethod = null;

    this._sink = Cc["@mozilla.org/messenger/pop3-sink;1"].createInstance(
      Ci.nsIPop3Sink
    );
    this._sink.popServer = server;

    this._logger = console.createInstance({
      prefix: "mailnews.pop3",
      maxLogLevel: "Warn",
      maxLogLevelPref: "mailnews.pop3.loglevel",
    });

    this.onReady = () => {};

    this._cutOffTimestamp = -1;
    if (
      this._server.deleteByAgeFromServer &&
      this._server.numDaysToLeaveOnServer
    ) {
      // We will send DELE request for messages received before this timestamp.
      this._cutOffTimestamp =
        Date.now() / 1000 - this._server.numDaysToLeaveOnServer * 24 * 60 * 60;
    }

    this._maxMessageSize = 50 * 1024;
    if (this._server.limitOfflineMessageSize && this._server.maxMessageSize) {
      this._maxMessageSize = this._server.maxMessageSize * 1024;
    }
  }

  /**
   * Initiate a connection to the server
   */
  connect() {
    this._logger.debug(
      `Connecting to pop://${this._server.realHostName}:${this._server.port}`
    );
    this._server.serverBusy = true;
    this._secureTransport = this._server.socketType == Ci.nsMsgSocketType.SSL;
    this._socket = new TCPSocket(this._server.realHostName, this._server.port, {
      binaryType: "arraybuffer",
      useSecureTransport: this._secureTransport,
    });
    this._socket.onopen = this._onOpen;
    this._socket.onerror = this._onError;

    this._authenticating = false;
    // Indicates if the connection has been closed and can't be used anymore.
    this._destroyed = false;
  }

  /**
   * Check and fetch new mails.
   * @param {nsIMsgWindow} msgWindow - The associated msg window.
   * @param {nsIUrlListener} urlListener - Callback for the request.
   * @param {nsIMsgFolder} folder - The folder to save the messages to.
   */
  async getMail(msgWindow, urlListener, folder) {
    this._msgWindow = msgWindow;
    this._urlListener = urlListener;
    this._sink.folder = folder;
    this._actionAfterAuth = this._actionStat;

    await this._loadUidlState();
    if (this._server.pop3CapabilityFlags & POP3_AUTH_MECH_UNDEFINED) {
      this._actionInitialAuth();
    } else {
      this._actionCapa();
    }
  }

  verifyLogon(msgWindow, urlListener) {
    this._msgWindow = msgWindow;
    this._urlListener = urlListener;
    this._verifyLogon = true;
    this._actionAfterAuth = this._actionDone;
    this._actionCapa();
  }

  /**
   * Send `QUIT` request to the server.
   */
  quit() {
    this._send("QUIT");
    this._nextAction = this.close;
  }

  /**
   * Close the socket.
   */
  close() {
    this._socket.close();
  }

  /**
   * The open event handler.
   */
  _onOpen = () => {
    this._logger.debug("Connected");
    this._socket.ondata = this._onData;
    this._socket.onclose = this._onClose;
    this._nextAction = () => {
      this.onOpen();
    };
  };

  /**
   * Parse the server response.
   * @param {string} str - Response received from the server.
   * @returns {Pop3Response}
   */
  _parse(str) {
    let matches = /^(\+OK|-ERR|\+) ?(.*)\r\n([^]*)/.exec(str);
    if (matches) {
      let [, status, statusText, data] = matches;
      return { success: status != "-ERR", status, statusText, data };
    }
    return { data: str };
  }

  /**
   * The data event handler.
   * @param {TCPSocketEvent} event - The data event.
   */
  _onData = async event => {
    // Some servers close the socket on invalid username/password, this line
    // guarantees onclose is handled before we try another AUTH method. See the
    // same handling in SmtpClient.jsm.
    await new Promise(resolve => setTimeout(resolve));

    let stringPayload = CommonUtils.arrayBufferToByteString(
      new Uint8Array(event.data)
    );
    this._logger.debug(`S: ${stringPayload}`);
    let res = this._parse(stringPayload);
    this._nextAction?.(res);
  };

  /**
   * The error event handler.
   * @param {TCPSocketErrorEvent} event - The error event.
   */
  _onError = event => {
    this._logger.error(event, event.name, event.message, event.errorCode);
    let secInfo = event.target.transport?.securityInfo;
    if (secInfo) {
      this.runningUri.failedSecInfo = secInfo;
    }
    this._actionDone(event.errorCode);
  };

  /**
   * The close event handler.
   */
  _onClose = () => {
    this._logger.debug("Connection closed.");
    this._server.serverBusy = false;
    this._destroyed = true;
    if (this._authenticating) {
      // In some cases, socket is closed for invalid username/password.
      this._actionAuthResponse({ success: false });
    }
  };

  _lineSeparator = AppConstants.platform == "win" ? "\r\n" : "\n";

  /**
   * Read popstate.dat into this._uidlMap.
   */
  async _loadUidlState() {
    this._uidlMap = new Map();
    let stateFile = this._server.localPath;
    stateFile.append("popstate.dat");
    if (!(await IOUtils.exists(stateFile.path))) {
      return;
    }

    let content = await IOUtils.readUTF8(stateFile.path);
    let uidlLine = false;
    for (let line of content.split(this._lineSeparator)) {
      if (!line) {
        continue;
      }
      if (uidlLine) {
        let [status, uidl, receivedAt] = line.split(" ");
        this._uidlMap.set(uidl, {
          // 'k'=KEEP, 'd'=DELETE, 'b'=TOO_BIG, 'f'=FETCH_BODY
          status,
          uidl,
          receivedAt,
        });
      }
      if (line.startsWith("#")) {
        // A comment line.
        continue;
      }
      if (line.startsWith("*")) {
        // The host & user line.
        uidlLine = true;
      }
    }
  }

  /**
   * Write this._uidlMap into popstate.dat.
   */
  async _writeUidlState() {
    if (!this._uidlMapChanged) {
      return;
    }

    let stateFile = this._server.localPath;
    stateFile.append("popstate.dat");
    let content = [
      "# POP3 State File",
      "# This is a generated file!  Do not edit.",
      "",
      `*${this._server.realHostName} ${this._server.realUsername}`,
    ];
    for (let { status, uidl, receivedAt } of this._uidlMap.values()) {
      content.push(`${status} ${uidl} ${receivedAt}`);
    }
    await IOUtils.writeUTF8(stateFile.path, content.join(this._lineSeparator));

    this._uidlMapChanged = false;
  }

  /**
   * Read multi-line data blocks response, emit each line through a callback.
   * @param {string} data - Response received from the server.
   * @param {Function} lineCallback - A line will be passed to the callback each
   *   time.
   * @param {Function} doneCallback - A function to be called when data is ended.
   */
  _lineReader(data, lineCallback, doneCallback) {
    if (this._leftoverData) {
      // For a single request, the response can span multiple ondata events.
      // Concatenate the leftover data from last event to the current data.
      data = this._leftoverData + data;
      this._leftoverData = null;
    }
    let ended = false;
    if (data == ".\r\n" || data.endsWith("\r\n.\r\n")) {
      ended = true;
      data = data.slice(0, -3);
    }
    while (data) {
      let index = data.indexOf("\r\n");
      if (index == -1) {
        // Not enough data, save it for the next round.
        this._leftoverData = data;
        break;
      }
      let line = data.slice(0, index + 2);
      if (line.startsWith("..")) {
        // Remove stuffed dot.
        line = line.slice(1);
      }
      lineCallback(line);
      data = data.slice(index + 2);
    }
    if (ended) {
      doneCallback(null);
    }
  }

  /**
   * Send a command to the server.
   * @param {string} str - The command string to send.
   * @param {boolean} [suppressLogging=false] - Whether to suppress logging the str.
   */
  _send(str, suppressLogging) {
    if (suppressLogging && AppConstants.MOZ_UPDATE_CHANNEL != "default") {
      this._logger.debug(
        "C: Logging suppressed (it probably contained auth information)"
      );
    } else {
      // Do not suppress for non-release builds, so that debugging auth problems
      // is easier.
      this._logger.debug(`C: ${str}`);
    }

    if (this._socket.readyState != "open") {
      this._logger.warn(
        `Failed to send because socket state is ${this._socket.readyState}`
      );
      return;
    }

    this._socket.send(CommonUtils.byteStringToArrayBuffer(str + "\r\n").buffer);
  }

  /**
   * Send `AUTH` request without any parameters to the server, to get supported
   * auth methods in case CAPA is not implemented by the server.
   */
  _actionInitialAuth = () => {
    this._nextAction = this._actionInitialAuthResponse;
    this._send("AUTH");
  };

  /**
   * Handle `AUTH` response.
   * @param {Pop3Response} res - AUTH response received from the server.
   */
  _actionInitialAuthResponse = res => {
    if (!res.success) {
      this._actionCapa();
    }
    this._lineReader(
      res.data,
      line => {
        this._supportedAuthMethods.push(line);
      },
      () => {
        // Clear the capability flags so that _actionInitialAuth is not needed
        // next time, this is only here to make tests happy.
        this._server.pop3CapabilityFlags = 0;
        this._actionCapa();
      }
    );
  };

  /**
   * Send `CAPA` request to the server.
   */
  _actionCapa = () => {
    this._nextAction = this._actionCapaResponse;
    this._hasSTLS = false;
    this._send("CAPA");
  };

  /**
   * Handle `CAPA` response.
   * @param {Pop3Response} res - CAPA response received from the server.
   */
  _actionCapaResponse = res => {
    if (!res.success) {
      this._actionChooseFirstAuthMethod();
    }
    this._lineReader(
      res.data,
      line => {
        if (line.startsWith("STLS")) {
          this._hasSTLS = true;
        }
        if (line.startsWith("SASL ")) {
          this._supportedAuthMethods = line
            .slice(5)
            .trim()
            .split(" ");
        }
      },
      () => this._actionChooseFirstAuthMethod()
    );
  };

  /**
   * Decide the first auth method to try.
   */
  _actionChooseFirstAuthMethod = () => {
    if (
      [
        Ci.nsMsgSocketType.trySTARTTLS,
        Ci.nsMsgSocketType.alwaysSTARTTLS,
      ].includes(this._server.socketType) &&
      !this._secureTransport
    ) {
      if (this._hasSTLS) {
        // Init STARTTLS negotiation if required by user pref and supported.
        this._nextAction = this._actionStlsResponse;
        // STLS is the POP3 command to init STARTTLS.
        this._send("STLS");
      } else {
        // Abort if not supported.
        this._logger.error("Server doesn't support STLS. Aborting.");
        this._actionDone(Cr.NS_ERROR_FAILURE);
      }
      return;
    }

    // If a preferred method is not supported by the server, no need to try it.
    this._possibleAuthMethods = this._preferredAuthMethods.filter(x =>
      this._supportedAuthMethods.includes(x)
    );
    this._logger.debug(`Possible auth methods: ${this._possibleAuthMethods}`);
    this._nextAuthMethod = this._nextAuthMethod || this._possibleAuthMethods[0];
    if (
      !this._supportedAuthMethods.length &&
      this._server.authMethod == Ci.nsMsgAuthMethod.passwordCleartext
    ) {
      this._possibleAuthMethods.unshift("USERPASS");
      this._nextAuthMethod = "USERPASS";
    }

    this._actionAuth();
  };

  /**
   * Handle STLS response. STLS is the POP3 command to init STARTTLS.
   * @param {Pop3Response} res - STLS response received from the server.
   */
  _actionStlsResponse = res => {
    if (!res.success) {
      this._actionDone(Cr.NS_ERROR_FAILURE);
      return;
    }
    this._socket.upgradeToSecure();
    this._secureTransport = true;
    this._actionCapa();
  };

  /**
   * Init authentication depending on server capabilities and user prefs.
   */
  _actionAuth = async () => {
    if (!this._nextAuthMethod) {
      this._actionDone(Cr.NS_ERROR_FAILURE);
      return;
    }

    if (this._destroyed) {
      // If connection is lost, reconnect.
      this.connect();
      return;
    }

    this._authenticating = true;

    this._currentAuthMethod = this._nextAuthMethod;
    this._nextAuthMethod = this._possibleAuthMethods[
      this._possibleAuthMethods.indexOf(this._currentAuthMethod) + 1
    ];
    this._logger.debug(`Current auth method: ${this._currentAuthMethod}`);
    this._nextAction = this._actionAuthResponse;

    switch (this._currentAuthMethod) {
      case "USERPASS":
        this._nextAction = this._actionAuthUserPass;
        this._send(`USER ${this._authenticator.username}`);
        break;
      case "PLAIN":
        this._nextAction = this._actionAuthPlain;
        this._send("AUTH PLAIN");
        break;
      case "LOGIN":
        this._nextAction = this._actionAuthLoginUser;
        this._send("AUTH LOGIN");
        break;
      case "CRAM-MD5":
        this._nextAction = this._actionAuthCramMd5;
        this._send("AUTH CRAM-MD5");
        break;
      case "GSSAPI": {
        this._nextAction = this._actionAuthGssapi;
        this._authenticator.initGssapiAuth("pop");
        let token;
        try {
          token = this._authenticator.getNextGssapiToken("");
        } catch (e) {
          this._logger.error(e);
          this._actionDone(Cr.NS_ERROR_FAILURE);
          return;
        }
        this._send(`AUTH GSSAPI ${token}`, true);
        break;
      }
      case "NTLM": {
        this._nextAction = this._actionAuthNtlm;
        this._authenticator.initNtlmAuth("pop");
        let token;
        try {
          token = this._authenticator.getNextNtlmToken("");
        } catch (e) {
          this._logger.error(e);
          this._actionDone(Cr.NS_ERROR_FAILURE);
        }
        this._send(`AUTH NTLM ${token}`, true);
        break;
      }
      case "XOAUTH2":
        this._nextAction = this._actionAuthResponse;
        let token = await this._authenticator.getOAuthToken();
        this._send(`AUTH XOAUTH2 ${token}`, true);
        break;
      default:
        this._actionDone();
    }
  };

  /**
   * Handle authentication response.
   * @param {Pop3Response} res - Authentication response received from the server.
   */
  _actionAuthResponse = res => {
    if (res.success) {
      this._authenticating = false;
      this._actionAfterAuth();
    } else {
      if (this._nextAuthMethod) {
        // Try the next auth method.
        this._actionAuth();
        return;
      }

      if (this._verifyLogon) {
        return;
      }

      // Ask user what to do.
      let action = this._authenticator.promptAuthFailed();
      if (action == 1) {
        // Cancel button pressed.
        this._actionDone(Cr.NS_ERROR_FAILURE);
        return;
      }
      if (action == 2) {
        // 'New password' button pressed.
        this._authenticator.forgetPassword();
      }

      // Retry.
      this._nextAuthMethod = this._possibleAuthMethods[0];
      this._actionAuth();
    }
  };

  /**
   * The second step of USER/PASS auth, send the password to the server.
   */
  _actionAuthUserPass = () => {
    this._nextAction = this._actionAuthResponse;
    this._send(`PASS ${this._authenticator.getPassword()}`, true);
  };

  /**
   * The second step of PLAIN auth, send the auth token to the server.
   */
  _actionAuthPlain = () => {
    this._nextAction = this._actionAuthResponse;
    let password = String.fromCharCode(
      ...new TextEncoder().encode(this._authenticator.getPassword())
    );
    this._send(
      btoa("\0" + this._authenticator.username + "\0" + password),
      true
    );
  };

  /**
   * The second step of LOGIN auth, send the username to the server.
   */
  _actionAuthLoginUser = () => {
    this._nextAction = this._actionAuthLoginPass;
    this._logger.debug("AUTH LOGIN USER");
    this._send(btoa(this._authenticator.username), true);
  };

  /**
   * The third step of LOGIN auth, send the password to the server.
   */
  _actionAuthLoginPass = () => {
    this._nextAction = this._actionAuthResponse;
    this._logger.debug("AUTH LOGIN PASS");
    let password = this._authenticator.getPassword();
    if (
      !Services.prefs.getBoolPref(
        "mail.smtp_login_pop3_user_pass_auth_is_latin1",
        true
      ) ||
      !/^[\x00-\xFF]+$/.test(password) // eslint-disable-line no-control-regex
    ) {
      // Unlike PLAIN auth, the payload of LOGIN auth is not standardized. When
      // `mail.smtp_login_pop3_user_pass_auth_is_latin1` is true, we apply
      // base64 encoding directly. Otherwise, we convert it to UTF-8
      // BinaryString first.
      password = String.fromCharCode(...new TextEncoder().encode(password));
    }
    this._send(btoa(password), true);
  };

  /**
   * The second step of CRAM-MD5 auth, send a HMAC-MD5 signature to the server.
   * @param {Pop3Response} res - AUTH response received from the server.
   */
  _actionAuthCramMd5 = res => {
    this._nextAction = this._actionAuthResponse;

    // Server sent us a base64 encoded challenge.
    let challenge = atob(res.statusText);
    let password = this._authenticator.getPassword();
    // Use password as key, challenge as payload, generate a HMAC-MD5 signature.
    let signature = MailCryptoUtils.hmacMd5(
      new TextEncoder().encode(password),
      new TextEncoder().encode(challenge)
    );
    // Get the hex form of the signature.
    let hex = [...signature].map(x => x.toString(16).padStart(2, "0")).join("");
    // Send the username and signature back to the server.
    this._send(btoa(`${this._authenticator.username} ${hex}`), true);
  };

  /**
   * The second and next step of GSSAPI auth.
   * @param {Pop3Response} res - AUTH response received from the server.
   */
  _actionAuthGssapi = res => {
    if (res.status != "+") {
      this._actionAuthResponse(res);
      return;
    }

    // Server returns a challenge, we send a new token. Can happen multiple times.
    let token;
    try {
      token = this._authenticator.getNextGssapiToken(res.statusText);
    } catch (e) {
      this._logger.error(e);
      this._actionAuthResponse({ success: false, data: "AUTH GSSAPI" });
      return;
    }
    this._send(token, true);
  };

  /**
   * The second and next step of NTLM auth.
   * @param {Pop3Response} res - AUTH response received from the server.
   */
  _actionAuthNtlm = res => {
    if (res.status != "+") {
      this._actionAuthResponse(res);
      return;
    }

    // Server returns a challenge, we send a new token. Can happen multiple times.
    let token;
    try {
      token = this._authenticator.getNextNtlmToken(res.statusText);
    } catch (e) {
      this._logger.error(e);
      this._actionAuthResponse({ success: false, data: "AUTH NTLM" });
      return;
    }
    this._send(token, true);
  };

  /**
   * Send `STAT` request to the server.
   */
  _actionStat = () => {
    this._nextAction = this._actionStatResponse;
    this._send("STAT");
  };

  /**
   * Handle `STAT` response.
   * @param {Pop3Response} res - STAT response received from the server.
   */
  _actionStatResponse = res => {
    if (!Number.parseInt(res.statusText)) {
      // Finish if there is no message.
      this._actionDone();
      return;
    }
    if (res.success) {
      this._actionList();
    }
  };

  /**
   * Send `LIST` request to the server.
   */
  _actionList = () => {
    this._messageSizeMap = new Map();
    this._nextAction = this._actionListResponse;
    this._send("LIST");
  };

  /**
   * Handle `LIST` response.
   * @param {Pop3Response} res - LIST response received from the server.
   */
  _actionListResponse = ({ data }) => {
    this._lineReader(
      data,
      line => {
        let [messageNumber, messageSize] = line.split(" ");
        this._messageSizeMap.set(messageNumber, Number(messageSize));
      },
      () => {
        this._actionUidl();
      }
    );
  };

  /**
   * Send `UIDL` request to the server.
   */
  _actionUidl = () => {
    this._messagesToHandle = [];
    this._newUidlMap = new Map();
    this._nextAction = this._actionUidlResponse;
    this._send("UIDL");
  };

  /**
   * Handle `UIDL` response.
   * @param {Pop3Response} res - UIDL response received from the server.
   */
  _actionUidlResponse = ({ data }) => {
    this._lineReader(
      data,
      line => {
        let [messageNumber, messageUidl] = line.split(" ");
        messageUidl = messageUidl.trim();
        let uidlState = this._uidlMap.get(messageUidl);
        if (uidlState) {
          if (
            uidlState.status == "k" &&
            (!this._server.leaveMessagesOnServer ||
              uidlState.receivedAt < this._cutOffTimestamp)
          ) {
            // Delete this message.
            this._messagesToHandle.push({
              messageNumber,
              messageUidl,
              status: "d",
            });
          } else {
            // Do nothing to this message.
            this._newUidlMap.set(messageUidl, uidlState);
          }
        } else {
          // Fetch the full message or only headers depending on server settings
          // and message size.
          let status =
            this._server.headersOnly ||
            this._messageSizeMap.get(messageNumber) > this._maxMessageSize
              ? "b"
              : "f";
          this._messagesToHandle.push({
            messageNumber,
            messageUidl,
            status,
          });
        }
      },
      () => {
        this._uidlMapChanged =
          this._uidlMap.size != this._newUidlMap.size ||
          this._messagesToHandle.length;
        // This discards staled uidls that are no longer on the server.
        this._uidlMap = this._newUidlMap;

        this._actionHandleMessage();
      }
    );
  };

  /**
   * Consume a message from this._messagesToHandle, decide to send TOP, RETR or
   * DELE request.
   */
  _actionHandleMessage = () => {
    this._currentMessage = this._messagesToHandle.shift();
    if (this._currentMessage) {
      switch (this._currentMessage.status) {
        case "b":
          this._actionTop();
          break;
        case "f":
          this._actionRetr();
          break;
        case "d":
          this._actionDelete();
          break;
        default:
          break;
      }
    } else {
      this._actionDone();
    }
  };

  /**
   * Send `TOP` request to the server.
   */
  _actionTop = () => {
    this._nextAction = this._actionTopResponse;
    let lineNumber = this._server.headersOnly ? 0 : 10;
    this._send(`TOP ${this._currentMessage.messageNumber} ${lineNumber}`);
  };

  /**
   * Handle `TOP` response.
   * @param {Pop3Response} res - TOP response received from the server.
   */
  _actionTopResponse = res => {
    if (!this._currentMessageSize) {
      // Call incorporateBegin only once for each message.
      this._sink.incorporateBegin(
        this._currentMessage.messageUidl,
        Ci.nsMsgMessageFlags.Partial
      );
    }
    if (res.statusText) {
      this._currentMessageSize = Number.parseInt(res.statusText);
    }
    this._lineReader(
      res.data,
      line => {
        this._sink.incorporateWrite(line, line.length);
      },
      () => {
        this._sink.incorporateComplete(
          this._msgWindow,
          this._currentMessageSize
        );
        this._currentMessageSize = null;
        this._uidlMap.set(this._currentMessage.messageUidl, {
          status: "b",
          uidl: this._currentMessage.messageUidl,
          receivedAt: Math.floor(Date.now() / 1000),
        });
        this._actionHandleMessage();
      }
    );
  };

  /**
   * Send `RETR` request to the server.
   */
  _actionRetr = () => {
    this._nextAction = this._actionRetrResponse;
    this._send(`RETR ${this._currentMessage.messageNumber}`);
  };

  /**
   * Handle `RETR` response.
   * @param {Pop3Response} res - RETR response received from the server.
   */
  _actionRetrResponse = res => {
    if (!this._currentMessageSize) {
      // Call incorporateBegin only once for each message.
      this._sink.incorporateBegin(this._currentMessage.messageUidl, 0);
    }
    if (res.statusText) {
      this._currentMessageSize = Number.parseInt(res.statusText);
    }
    this._lineReader(
      res.data,
      line => {
        this._sink.incorporateWrite(line, line.length);
      },
      () => {
        this._sink.incorporateComplete(
          this._msgWindow,
          this._currentMessageSize
        );
        this._currentMessageSize = null;
        if (this._server.leaveMessagesOnServer) {
          this._uidlMap.set(this._currentMessage.messageUidl, {
            status: "k",
            uidl: this._currentMessage.messageUidl,
            receivedAt: Math.floor(Date.now() / 1000),
          });
          this._actionHandleMessage();
        } else {
          this._actionDelete();
        }
      }
    );
  };

  /**
   * Send `DELE` request to the server.
   */
  _actionDelete = () => {
    this._nextAction = this._actionDeleteResponse;
    this._send(`DELE ${this._currentMessage.messageNumber}`);
  };

  /**
   * Handle `DELE` response.
   * @param {Pop3Response} res - DELE response received from the server.
   */
  _actionDeleteResponse = res => {
    this._actionHandleMessage();
  };

  _actionDone = (status = Cr.NS_OK) => {
    this._authenticating = false;
    this.quit();
    this._writeUidlState();
    this._urlListener.OnStopRunningUrl(this.runningUri, status);
  };
}
