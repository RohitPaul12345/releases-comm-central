/* -*- Mode: Javascript; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*  This file contains the js functions necessary to implement view navigation within the 3 pane. */

/* globals gDBView */ // mailCommon.js

ChromeUtils.defineModuleGetter(
  this,
  "FolderUtils",
  "resource:///modules/FolderUtils.jsm"
);

function GetSubFoldersInFolderPaneOrder(folder) {
  function compareFolderSortKey(folder1, folder2) {
    return folder1.compareSortKeys(folder2);
  }
  // sort the subfolders
  return folder.subFolders.sort(compareFolderSortKey);
}

function FindNextChildFolder(aParent, aAfter) {
  // Search the child folders of aParent for unread messages
  // but in the case that we are working up from the current folder
  // we need to skip up to and including the current folder
  // we skip the current folder in case a mail view is hiding unread messages
  if (aParent.getNumUnread(true) > 0) {
    var subFolders = GetSubFoldersInFolderPaneOrder(aParent);
    var i = 0;
    var folder = null;

    // Skip folders until after the specified child
    while (folder != aAfter) {
      folder = subFolders[i++];
    }

    let ignoreFlags =
      Ci.nsMsgFolderFlags.Trash |
      Ci.nsMsgFolderFlags.SentMail |
      Ci.nsMsgFolderFlags.Drafts |
      Ci.nsMsgFolderFlags.Queue |
      Ci.nsMsgFolderFlags.Templates |
      Ci.nsMsgFolderFlags.Junk;
    while (i < subFolders.length) {
      folder = subFolders[i++];
      // If there is unread mail in the trash, sent, drafts, unsent messages
      // templates or junk special folder,
      // we ignore it when doing cross folder "next" navigation.
      if (!folder.isSpecialFolder(ignoreFlags, true)) {
        if (folder.getNumUnread(false) > 0) {
          return folder;
        }

        folder = FindNextChildFolder(folder, null);
        if (folder) {
          return folder;
        }
      }
    }
  }

  return null;
}

function FindNextFolder() {
  // look for the next folder, this will only look on the current account
  // and below us, in the folder pane
  // note use of gDBView restricts this function to message folders
  // otherwise you could go next unread from a server
  var folder = FindNextChildFolder(gDBView.msgFolder, null);
  if (folder) {
    return folder;
  }

  // didn't find folder in children
  // go up to the parent, and start at the folder after the current one
  // unless we are at a server, in which case bail out.
  folder = gDBView.msgFolder;
  while (!folder.isServer) {
    var parent = folder.parent;
    folder = FindNextChildFolder(parent, folder);
    if (folder) {
      return folder;
    }

    // none at this level after the current folder.  go up.
    folder = parent;
  }

  // nothing in the current account, start with the next account (below)
  // and try until we hit the bottom of the folder pane

  // start at the account after the current account
  var rootFolders = GetRootFoldersInFolderPaneOrder();
  for (var i = 0; i < rootFolders.length; i++) {
    if (rootFolders[i].URI == gDBView.msgFolder.server.serverURI) {
      break;
    }
  }

  for (var j = i + 1; j < rootFolders.length; j++) {
    folder = FindNextChildFolder(rootFolders[j], null);
    if (folder) {
      return folder;
    }
  }

  // if nothing from the current account down to the bottom
  // (of the folder pane), start again at the top.
  for (j = 0; j <= i; j++) {
    folder = FindNextChildFolder(rootFolders[j], null);
    if (folder) {
      return folder;
    }
  }
  return null;
}

function GetRootFoldersInFolderPaneOrder() {
  let accounts = FolderUtils.allAccountsSorted(false);

  let serversMsgFolders = [];
  for (let account of accounts) {
    serversMsgFolders.push(account.incomingServer.rootMsgFolder);
  }

  return serversMsgFolders;
}

/**
 * Handle switching the folder if required for the given kind of navigation.
 * Only used in about:3pane.
 *
 * @param {nsMsgNavigationType} type - The type of navigation.
 * @returns {boolean} If the folder was changed for the navigation.
 */
function CrossFolderNavigation(type) {
  // do cross folder navigation for next unread message/thread and message history
  if (
    type != Ci.nsMsgNavigationType.nextUnreadMessage &&
    type != Ci.nsMsgNavigationType.nextUnreadThread &&
    type != Ci.nsMsgNavigationType.forward &&
    type != Ci.nsMsgNavigationType.back
  ) {
    return false;
  }

  if (
    type == Ci.nsMsgNavigationType.nextUnreadMessage ||
    type == Ci.nsMsgNavigationType.nextUnreadThread
  ) {
    var nextMode = Services.prefs.getIntPref("mailnews.nav_crosses_folders");
    // 0: "next" goes to the next folder, without prompting
    // 1: "next" goes to the next folder, and prompts (the default)
    // 2: "next" does nothing when there are no unread messages

    // not crossing folders, don't find next
    if (nextMode == 2) {
      return false;
    }

    var folder = FindNextFolder();
    if (folder && gDBView.msgFolder.URI != folder.URI) {
      if (nextMode == 1) {
        let messengerBundle =
          window.messengerBundle ||
          Services.strings.createBundle(
            "chrome://messenger/locale/messenger.properties"
          );
        let promptText = messengerBundle.formatStringFromName(
          "advanceNextPrompt",
          [folder.name]
        );
        if (
          Services.prompt.confirmEx(
            window,
            null,
            promptText,
            Services.prompt.STD_YES_NO_BUTTONS,
            null,
            null,
            null,
            null,
            {}
          )
        ) {
          return false;
        }
      }
      window.threadPane.forgetSelection(folder.URI);
      window.displayFolder(folder.URI);
      return true;
    }
  } else {
    let { messageHistory } = window.messageBrowser.contentWindow;
    let relPos = -1;
    if (type == Ci.nsMsgNavigationType.forward) {
      relPos = 1;
    } else if (messageHistory.canPop(0)) {
      relPos = 0;
    }
    let folderURI = messageHistory.getMessageAt(relPos)?.folderURI;
    if (!folderURI || window.gFolder?.URI === folderURI) {
      return false;
    }

    window.threadPane.forgetSelection(folderURI);
    window.displayFolder(folderURI);
    return true;
  }

  return false;
}
