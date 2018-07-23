/* -*- indent-tabs-mode: nil; js-indent-level: 2 -*-
 * vim: sw=2 ts=2 sts=2 et filetype=javascript
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

var EXPORTED_SYMBOLS = [
  "DownloadsTaskbar",
];

ChromeUtils.import("resource://gre/modules/XPCOMUtils.jsm");

ChromeUtils.defineModuleGetter(this, "Downloads",
                               "resource://gre/modules/Downloads.jsm");
ChromeUtils.defineModuleGetter(this, "Services",
                               "resource://gre/modules/Services.jsm");

XPCOMUtils.defineLazyGetter(this, "gWinTaskbar", function() {
  if (!("@mozilla.org/windows-taskbar;1" in Cc)) {
    return null;
  }
  let winTaskbar = Cc["@mozilla.org/windows-taskbar;1"]
                     .getService(Ci.nsIWinTaskbar);
  return winTaskbar.available && winTaskbar;
});

XPCOMUtils.defineLazyGetter(this, "gMacTaskbarProgress", function() {
  return ("@mozilla.org/widget/macdocksupport;1" in Cc) &&
         Cc["@mozilla.org/widget/macdocksupport;1"]
           .getService(Ci.nsITaskbarProgress);
});

// DownloadsTaskbar

/**
 * Handles the download progress indicator in the taskbar.
 */
var DownloadsTaskbar = {
  /**
   * Underlying DownloadSummary providing the aggregate download information, or
   * null if the indicator has never been initialized.
   */
  _summary: null,

  /**
   * nsITaskbarProgress object to which download information is dispatched.
   * This can be null if the indicator has never been initialized or if the
   * indicator is currently hidden on Windows.
   */
  _taskbarProgress: null,

  /**
   * This method is called after a new browser window is opened, and ensures
   * that the download progress indicator is displayed in the taskbar.
   *
   * On Windows, the indicator is attached to the first browser window that
   * calls this method.  When the window is closed, the indicator is moved to
   * another browser window, if available, in no particular order.  When there
   * are no browser windows visible, the indicator is hidden.
   *
   * On Mac OS X, the indicator is initialized globally when this method is
   * called for the first time.  Subsequent calls have no effect.
   *
   * @param aBrowserWindow
   *        nsIDOMWindow object of the newly opened browser window to which the
   *        indicator may be attached.
   */
  registerIndicator(aWindow) {
    if (!this._taskbarProgress) {
      if (gMacTaskbarProgress) {
        // On Mac OS X, we have to register the global indicator only once.
        this._taskbarProgress = gMacTaskbarProgress;
        // Free the XPCOM reference on shutdown, to prevent detecting a leak.
        Services.obs.addObserver(() => {
          this._taskbarProgress = null;
          gMacTaskbarProgress = null;
        }, "quit-application-granted");
      } else if (gWinTaskbar) {
        // On Windows, the indicator is currently hidden because we have no
        // previous window, thus we should attach the indicator now.
        this.attachIndicator(aWindow);
      } else {
        // The taskbar indicator is not available on this platform.
        return;
      }
    }

    // Ensure that the DownloadSummary object will be created asynchronously.
    if (!this._summary) {
      Downloads.getSummary(Downloads.ALL).then(summary => {
        // In case the method is re-entered, we simply ignore redundant
        // invocations of the callback, instead of keeping separate state.
        if (this._summary) {
          return undefined;
        }
        this._summary = summary;
        return this._summary.addView(this);
      }).catch(Cu.reportError);
    }
  },

  /**
   * On Windows, attaches the taskbar indicator to the specified window.
   */
  attachIndicator(aWindow) {
    // If there is already a taskbarProgress this usually means the download
    //  manager became active. So clear the taskbar state first.
    if (this._taskbarProgress) {
      this._taskbarProgress.setProgressState(Ci.nsITaskbarProgress.STATE_NO_PROGRESS);
    }

    // Activate the indicator on the specified window.
    let docShell = aWindow.QueryInterface(Ci.nsIInterfaceRequestor)
                          .getInterface(Ci.nsIWebNavigation)
                          .QueryInterface(Ci.nsIDocShellTreeItem).treeOwner
                          .QueryInterface(Ci.nsIInterfaceRequestor)
                          .getInterface(Ci.nsIXULWindow).docShell;
    this._taskbarProgress = gWinTaskbar.getTaskbarProgress(docShell);

    // If the DownloadSummary object has already been created, we should update
    // the state of the new indicator, otherwise it will be updated as soon as
    // the DownloadSummary view is registered.
    if (this._summary) {
      this.onSummaryChanged();
    }

    aWindow.addEventListener("unload", () => {
      let windows = Services.wm.getEnumerator(null);
      let newActiveWindow = null;
      if (windows.hasMoreElements()) {
        newActiveWindow = windows.getNext().QueryInterface(Ci.nsIDOMWindow);
      }
      if (newActiveWindow) {
        // Move the progress indicator to the other browser window.
        this.attachIndicator(newActiveWindow, false);
      } else {
        // The last window has been closed. We remove the reference to
        // the taskbar progress object.
        this._taskbarProgress = null;
      }
    });
  },

  // DownloadSummary view
  onSummaryChanged() {
    // If the last browser window has been closed, we have no indicator any more.
    if (!this._taskbarProgress) {
      return;
    }

    if (this._summary.allHaveStopped || this._summary.progressTotalBytes == 0) {
      this._taskbarProgress.setProgressState(
                               Ci.nsITaskbarProgress.STATE_NO_PROGRESS, 0, 0);
    } else {
      // For a brief moment before completion, some download components may
      // report more transferred bytes than the total number of bytes.  Thus,
      // ensure that we never break the expectations of the progress indicator.
      let progressCurrentBytes = Math.min(this._summary.progressTotalBytes,
                                          this._summary.progressCurrentBytes);
      this._taskbarProgress.setProgressState(
                               Ci.nsITaskbarProgress.STATE_NORMAL,
                               progressCurrentBytes,
                               this._summary.progressTotalBytes);
    }
  },
};
