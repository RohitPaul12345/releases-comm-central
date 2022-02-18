/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

var { Services } = ChromeUtils.import("resource://gre/modules/Services.jsm");
var { MailServices } = ChromeUtils.import(
  "resource:///modules/MailServices.jsm"
);
var { ThunderbirdProfileImporter } = ChromeUtils.import(
  "resource:///modules/ThunderbirdProfileImporter.jsm"
);

let tmpProfileDir;
registerCleanupFunction(() => {
  tmpProfileDir?.remove(true);
});

/**
 * Create a temporary dir to use as the source profile dir. Write a prefs.js
 * into it.
 * @param {Array<[string, string]>} prefs - An array of tuples, each tuple is
 *   a pref represented as [prefName, prefValue].
 */
async function createTmpProfileWithPrefs(prefs) {
  tmpProfileDir?.remove(true);

  // Create a temporary dir.
  tmpProfileDir = Services.dirsvc.get("TmpD", Ci.nsIFile);
  tmpProfileDir.append("profile-tmp");
  tmpProfileDir.createUnique(Ci.nsIFile.DIRECTORY_TYPE, 0o755);
  info(`Created a temporary profile at ${tmpProfileDir.path}`);

  // Write prefs to prefs.js.
  let prefsFile = tmpProfileDir.clone();
  prefsFile.append("prefs.js");
  let prefsContent = prefs
    .map(([name, value]) => {
      let prefValue = typeof value == "string" ? `"${value}"` : value;
      return `user_pref("${name}", ${prefValue});`;
    })
    .join("\n");
  return IOUtils.writeUTF8(prefsFile.path, prefsContent);
}

/**
 * Construct a temporary profile dir with prefs, import into the current
 * profile, then check the values of prefs related to mail accounts.
 */
add_task(async function test_importAccountsIntoEmptyProfile() {
  equal(
    Services.prefs.getCharPref("mail.accountmanager.accounts"),
    "",
    "Should have no accounts at first"
  );
  let charPrefs = [
    ["mail.smtpserver.smtp1.username", "smtp-user-1"],
    ["mail.smtpserver.smtp3.username", "smtp-user-2"],
    ["mail.smtpservers", "smtp1,smtp3"],
    ["mail.identity.id1.smtpServer", "smtp3"],
    ["mail.identity.id3.fullName", "id-name-2"],
    ["mail.identity.id4.stmpServer", "smtp1"],
    ["mail.server.server2.type", "none"],
    ["mail.server.server6.type", "imap"],
    ["mail.server.server7.type", "pop3"],
    ["mail.account.account2.server", "server2"],
    ["mail.account.account3.server", "server6"],
    ["mail.account.account3.identities", "id1,id3"],
    ["mail.account.account4.server", "server7"],
    ["mail.accountmanager.accounts", "account3,account4,account2"],
  ];
  await createTmpProfileWithPrefs(charPrefs);

  let importer = new ThunderbirdProfileImporter();

  await importer.startImport(tmpProfileDir, importer.SUPPORTED_ITEMS);
  // Server/identity/account keys should be changed and remapped correctly after
  // import.
  let expectedCharPrefs = [
    ["mail.smtpserver.smtp1.username", "smtp-user-1"],
    ["mail.smtpserver.smtp2.username", "smtp-user-2"],
    ["mail.smtpservers", "smtp1,smtp2"],
    ["mail.identity.id1.smtpServer", "smtp2"],
    ["mail.identity.id2.fullName", "id-name-2"],
    ["mail.identity.id3.stmpServer", "smtp1"],
    ["mail.server.server1.type", "none"],
    ["mail.server.server2.type", "imap"],
    ["mail.server.server3.type", "pop3"],
    ["mail.account.account1.server", "server1"],
    ["mail.account.account2.server", "server2"],
    ["mail.account.account2.identities", "id1,id2"],
    ["mail.account.account3.server", "server3"],
    ["mail.accountmanager.accounts", "account2,account3,account1"],
  ];
  for (let [name, value] of expectedCharPrefs) {
    equal(
      Services.prefs.getCharPref(name, ""),
      value,
      `${name} should be correct`
    );
  }

  // Remove all the prefs to do the next test.
  Services.prefs.resetPrefs();

  equal(
    Services.prefs.getCharPref("mail.accountmanager.accounts"),
    "",
    "Should have no accounts after resetPrefs"
  );
  await importer.startImport(tmpProfileDir, {
    ...importer.SUPPORTED_ITEMS,
    accounts: false,
    mailMessages: false, // If true, Local Folders is created
  });
  equal(
    Services.prefs.getCharPref("mail.accountmanager.accounts"),
    "",
    "Should still have no accounts without importing accounts"
  );

  Services.prefs.resetPrefs();
});

/**
 * Test that when the source profile and current profile each has Local Folders,
 * the source Local Folders will be merged into the current Local Folders.
 */
add_task(async function test_mergeLocalFolders() {
  let prefs = [
    ["mail.smtpserver.smtp1.username", "smtp-user-1"],
    ["mail.smtpservers", "smtp1"],
    ["mail.identity.id1.smtpServer", "smtp1"],
    ["mail.server.server2.type", "none"],
    ["mail.server.server2.directory-rel", "[ProfD]Mail/Local Folders"],
    ["mail.server.server2.hostname", "Local Folders"],
    ["mail.server.server3.type", "imap"],
    ["mail.account.account2.server", "server2"],
    ["mail.account.account3.server", "server3"],
    ["mail.account.account3.identities", "id1"],
    ["mail.accountmanager.accounts", "account3,account2"],
    ["mail.accountmanager.localfoldersserver", "server2"],
  ];
  await createTmpProfileWithPrefs(prefs);

  // Create a physical file in tmpProfileDir.
  let sourceLocalFolder = tmpProfileDir.clone();
  sourceLocalFolder.append("Mail");
  sourceLocalFolder.append("Local Folders");
  sourceLocalFolder.append("folder-xpcshell");
  sourceLocalFolder.create(Ci.nsIFile.NORMAL_FILE_TYPE, 0o644);

  // Create Local Folders in the current profile.
  MailServices.accounts.createLocalMailAccount();

  let importer = new ThunderbirdProfileImporter();
  await importer.startImport(tmpProfileDir, importer.SUPPORTED_ITEMS);

  // Test that sub msg folders are created in the current Local Folders.
  let localFolders = MailServices.accounts.localFoldersServer.rootMsgFolder;
  ok(localFolders.containsChildNamed("Local Folders"));
  let msgFolder = localFolders.getChildNamed("Local Folders");
  ok(msgFolder.containsChildNamed("folder-xpcshell"));

  // Test that folder-xpcshell is copied into current Local Folders.
  let importedFolder = localFolders.filePath;
  importedFolder.append("Local Folders.sbd");
  importedFolder.append("folder-xpcshell");
  ok(importedFolder.exists(), "Source Local Folders should be merged in.");
});
