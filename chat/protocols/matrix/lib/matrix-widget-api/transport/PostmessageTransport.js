"use strict";

function _typeof(obj) { "@babel/helpers - typeof"; return _typeof = "function" == typeof Symbol && "symbol" == typeof Symbol.iterator ? function (obj) { return typeof obj; } : function (obj) { return obj && "function" == typeof Symbol && obj.constructor === Symbol && obj !== Symbol.prototype ? "symbol" : typeof obj; }, _typeof(obj); }

Object.defineProperty(exports, "__esModule", {
  value: true
});
exports.PostmessageTransport = void 0;

var _events = require("events");

var _ = require("..");

function ownKeys(object, enumerableOnly) { var keys = Object.keys(object); if (Object.getOwnPropertySymbols) { var symbols = Object.getOwnPropertySymbols(object); enumerableOnly && (symbols = symbols.filter(function (sym) { return Object.getOwnPropertyDescriptor(object, sym).enumerable; })), keys.push.apply(keys, symbols); } return keys; }

function _objectSpread(target) { for (var i = 1; i < arguments.length; i++) { var source = null != arguments[i] ? arguments[i] : {}; i % 2 ? ownKeys(Object(source), !0).forEach(function (key) { _defineProperty(target, key, source[key]); }) : Object.getOwnPropertyDescriptors ? Object.defineProperties(target, Object.getOwnPropertyDescriptors(source)) : ownKeys(Object(source)).forEach(function (key) { Object.defineProperty(target, key, Object.getOwnPropertyDescriptor(source, key)); }); } return target; }

function _classCallCheck(instance, Constructor) { if (!(instance instanceof Constructor)) { throw new TypeError("Cannot call a class as a function"); } }

function _defineProperties(target, props) { for (var i = 0; i < props.length; i++) { var descriptor = props[i]; descriptor.enumerable = descriptor.enumerable || false; descriptor.configurable = true; if ("value" in descriptor) descriptor.writable = true; Object.defineProperty(target, descriptor.key, descriptor); } }

function _createClass(Constructor, protoProps, staticProps) { if (protoProps) _defineProperties(Constructor.prototype, protoProps); if (staticProps) _defineProperties(Constructor, staticProps); Object.defineProperty(Constructor, "prototype", { writable: false }); return Constructor; }

function _inherits(subClass, superClass) { if (typeof superClass !== "function" && superClass !== null) { throw new TypeError("Super expression must either be null or a function"); } subClass.prototype = Object.create(superClass && superClass.prototype, { constructor: { value: subClass, writable: true, configurable: true } }); Object.defineProperty(subClass, "prototype", { writable: false }); if (superClass) _setPrototypeOf(subClass, superClass); }

function _setPrototypeOf(o, p) { _setPrototypeOf = Object.setPrototypeOf ? Object.setPrototypeOf.bind() : function _setPrototypeOf(o, p) { o.__proto__ = p; return o; }; return _setPrototypeOf(o, p); }

function _createSuper(Derived) { var hasNativeReflectConstruct = _isNativeReflectConstruct(); return function _createSuperInternal() { var Super = _getPrototypeOf(Derived), result; if (hasNativeReflectConstruct) { var NewTarget = _getPrototypeOf(this).constructor; result = Reflect.construct(Super, arguments, NewTarget); } else { result = Super.apply(this, arguments); } return _possibleConstructorReturn(this, result); }; }

function _possibleConstructorReturn(self, call) { if (call && (_typeof(call) === "object" || typeof call === "function")) { return call; } else if (call !== void 0) { throw new TypeError("Derived constructors may only return object or undefined"); } return _assertThisInitialized(self); }

function _assertThisInitialized(self) { if (self === void 0) { throw new ReferenceError("this hasn't been initialised - super() hasn't been called"); } return self; }

function _isNativeReflectConstruct() { if (typeof Reflect === "undefined" || !Reflect.construct) return false; if (Reflect.construct.sham) return false; if (typeof Proxy === "function") return true; try { Boolean.prototype.valueOf.call(Reflect.construct(Boolean, [], function () {})); return true; } catch (e) { return false; } }

function _getPrototypeOf(o) { _getPrototypeOf = Object.setPrototypeOf ? Object.getPrototypeOf.bind() : function _getPrototypeOf(o) { return o.__proto__ || Object.getPrototypeOf(o); }; return _getPrototypeOf(o); }

function _defineProperty(obj, key, value) { if (key in obj) { Object.defineProperty(obj, key, { value: value, enumerable: true, configurable: true, writable: true }); } else { obj[key] = value; } return obj; }

/**
 * Transport for the Widget API over postMessage.
 */
var PostmessageTransport = /*#__PURE__*/function (_EventEmitter) {
  _inherits(PostmessageTransport, _EventEmitter);

  var _super = _createSuper(PostmessageTransport);

  function PostmessageTransport(sendDirection, initialWidgetId, transportWindow, inboundWindow) {
    var _this;

    _classCallCheck(this, PostmessageTransport);

    _this = _super.call(this);
    _this.sendDirection = sendDirection;
    _this.initialWidgetId = initialWidgetId;
    _this.transportWindow = transportWindow;
    _this.inboundWindow = inboundWindow;

    _defineProperty(_assertThisInitialized(_this), "strictOriginCheck", void 0);

    _defineProperty(_assertThisInitialized(_this), "targetOrigin", void 0);

    _defineProperty(_assertThisInitialized(_this), "timeoutSeconds", 10);

    _defineProperty(_assertThisInitialized(_this), "_ready", false);

    _defineProperty(_assertThisInitialized(_this), "_widgetId", null);

    _defineProperty(_assertThisInitialized(_this), "outboundRequests", new Map());

    _defineProperty(_assertThisInitialized(_this), "stopController", new AbortController());

    _this._widgetId = initialWidgetId;
    return _this;
  }

  _createClass(PostmessageTransport, [{
    key: "ready",
    get: function get() {
      return this._ready;
    }
  }, {
    key: "widgetId",
    get: function get() {
      return this._widgetId || null;
    }
  }, {
    key: "nextRequestId",
    get: function get() {
      var idBase = "widgetapi-".concat(Date.now());
      var index = 0;
      var id = idBase;

      while (this.outboundRequests.has(id)) {
        id = "".concat(idBase, "-").concat(index++);
      } // reserve the ID


      this.outboundRequests.set(id, null);
      return id;
    }
  }, {
    key: "sendInternal",
    value: function sendInternal(message) {
      var targetOrigin = this.targetOrigin || '*';
      console.log("[PostmessageTransport] Sending object to ".concat(targetOrigin, ": "), message);
      this.transportWindow.postMessage(message, targetOrigin);
    }
  }, {
    key: "reply",
    value: function reply(request, responseData) {
      return this.sendInternal(_objectSpread(_objectSpread({}, request), {}, {
        response: responseData
      }));
    }
  }, {
    key: "send",
    value: function send(action, data) {
      return this.sendComplete(action, data).then(function (r) {
        return r.response;
      });
    }
  }, {
    key: "sendComplete",
    value: function sendComplete(action, data) {
      var _this2 = this;

      if (!this.ready || !this.widgetId) {
        return Promise.reject(new Error("Not ready or unknown widget ID"));
      }

      var request = {
        api: this.sendDirection,
        widgetId: this.widgetId,
        requestId: this.nextRequestId,
        action: action,
        data: data
      };

      if (action === _.WidgetApiToWidgetAction.UpdateVisibility) {
        // XXX: This is for Scalar support
        // TODO: Fix scalar
        request['visible'] = data['visible'];
      }

      return new Promise(function (prResolve, prReject) {
        var resolve = function resolve(response) {
          cleanUp();
          prResolve(response);
        };

        var reject = function reject(err) {
          cleanUp();
          prReject(err);
        };

        var timerId = setTimeout(function () {
          return reject(new Error("Request timed out"));
        }, (_this2.timeoutSeconds || 1) * 1000);

        var onStop = function onStop() {
          return reject(new Error("Transport stopped"));
        };

        _this2.stopController.signal.addEventListener("abort", onStop);

        var cleanUp = function cleanUp() {
          _this2.outboundRequests["delete"](request.requestId);

          clearTimeout(timerId);

          _this2.stopController.signal.removeEventListener("abort", onStop);
        };

        _this2.outboundRequests.set(request.requestId, {
          request: request,
          resolve: resolve,
          reject: reject
        });

        _this2.sendInternal(request);
      });
    }
  }, {
    key: "start",
    value: function start() {
      var _this3 = this;

      this.inboundWindow.addEventListener("message", function (ev) {
        _this3.handleMessage(ev);
      });
      this._ready = true;
    }
  }, {
    key: "stop",
    value: function stop() {
      this._ready = false;
      this.stopController.abort();
    }
  }, {
    key: "handleMessage",
    value: function handleMessage(ev) {
      if (this.stopController.signal.aborted) return;
      if (!ev.data) return; // invalid event

      if (this.strictOriginCheck && ev.origin !== window.origin) return; // bad origin
      // treat the message as a response first, then downgrade to a request

      var response = ev.data;
      if (!response.action || !response.requestId || !response.widgetId) return; // invalid request/response

      if (!response.response) {
        // it's a request
        var request = response;
        if (request.api !== (0, _.invertedDirection)(this.sendDirection)) return; // wrong direction

        this.handleRequest(request);
      } else {
        // it's a response
        if (response.api !== this.sendDirection) return; // wrong direction

        this.handleResponse(response);
      }
    }
  }, {
    key: "handleRequest",
    value: function handleRequest(request) {
      if (this.widgetId) {
        if (this.widgetId !== request.widgetId) return; // wrong widget
      } else {
        this._widgetId = request.widgetId;
      }

      this.emit("message", new CustomEvent("message", {
        detail: request
      }));
    }
  }, {
    key: "handleResponse",
    value: function handleResponse(response) {
      if (response.widgetId !== this.widgetId) return; // wrong widget

      var req = this.outboundRequests.get(response.requestId);
      if (!req) return; // response to an unknown request

      if ((0, _.isErrorResponse)(response.response)) {
        var _err = response.response;
        req.reject(new Error(_err.error.message));
      } else {
        req.resolve(response);
      }
    }
  }]);

  return PostmessageTransport;
}(_events.EventEmitter);

exports.PostmessageTransport = PostmessageTransport;