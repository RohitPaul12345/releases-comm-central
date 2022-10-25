/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "msgCore.h"
#include "nsIMsgHdr.h"
#include "nsMsgUtils.h"
#include "nsMsgFolderFlags.h"
#include "nsMsgMessageFlags.h"
#include "nsString.h"
#include "nsIServiceManager.h"
#include "nsCOMPtr.h"
#include "nsIFolderLookupService.h"
#include "nsIImapUrl.h"
#include "nsIMailboxUrl.h"
#include "nsINntpUrl.h"
#include "nsMsgNewsCID.h"
#include "nsMsgLocalCID.h"
#include "nsMsgI18N.h"
#include "nsNativeCharsetUtils.h"
#include "nsCharTraits.h"
#include "prprf.h"
#include "prmem.h"
#include "nsNetCID.h"
#include "nsIIOService.h"
#include "nsIMimeConverter.h"
#include "nsMsgMimeCID.h"
#include "nsIPrefService.h"
#include "nsIPrefBranch.h"
#include "nsISupportsPrimitives.h"
#include "nsIPrefLocalizedString.h"
#include "nsIRelativeFilePref.h"
#include "mozilla/nsRelativeFilePref.h"
#include "nsAppDirectoryServiceDefs.h"
#include "nsISpamSettings.h"
#include "nsICryptoHash.h"
#include "nsNativeCharsetUtils.h"
#include "nsDirectoryServiceUtils.h"
#include "nsDirectoryServiceDefs.h"
#include "nsIRssIncomingServer.h"
#include "nsIMsgFolder.h"
#include "nsIMsgProtocolInfo.h"
#include "nsIMsgMessageService.h"
#include "nsIMsgAccountManager.h"
#include "nsIOutputStream.h"
#include "nsMsgFileStream.h"
#include "nsIFileURL.h"
#include "nsNetUtil.h"
#include "nsProtocolProxyService.h"
#include "nsIProtocolProxyCallback.h"
#include "nsICancelable.h"
#include "nsIMsgDatabase.h"
#include "nsIMsgMailNewsUrl.h"
#include "nsIStringBundle.h"
#include "nsIMsgWindow.h"
#include "nsIWindowWatcher.h"
#include "nsIPrompt.h"
#include "nsIMsgSearchTerm.h"
#include "nsTextFormatter.h"
#include "nsIStreamListener.h"
#include "nsReadLine.h"
#include "nsILineInputStream.h"
#include "nsIParserUtils.h"
#include "nsICharsetConverterManager.h"
#include "nsIDocumentEncoder.h"
#include "mozilla/Components.h"
#include "locale.h"
#include "nsStringStream.h"
#include "nsIInputStreamPump.h"
#include "nsIInputStream.h"
#include "nsIChannel.h"
#include "nsIURIMutator.h"
#include "mozilla/Unused.h"
#include "mozilla/Preferences.h"
#include "mozilla/Encoding.h"
#include "mozilla/EncodingDetector.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/Utf8.h"
#include "mozilla/Buffer.h"

/* for logging to Error Console */
#include "nsIScriptError.h"
#include "nsIConsoleService.h"

// Log an error string to the error console
// (adapted from nsContentUtils::LogSimpleConsoleError).
// Flag can indicate error, warning or info.
NS_MSG_BASE void MsgLogToConsole4(const nsAString& aErrorText,
                                  const nsAString& aFilename,
                                  uint32_t aLinenumber, uint32_t aFlag) {
  nsCOMPtr<nsIScriptError> scriptError =
      do_CreateInstance(NS_SCRIPTERROR_CONTRACTID);
  if (NS_WARN_IF(!scriptError)) return;
  nsCOMPtr<nsIConsoleService> console =
      do_GetService(NS_CONSOLESERVICE_CONTRACTID);
  if (NS_WARN_IF(!console)) return;
  if (NS_FAILED(scriptError->Init(aErrorText, aFilename, EmptyString(),
                                  aLinenumber, 0, aFlag, "mailnews"_ns, false,
                                  false)))
    return;
  console->LogMessage(scriptError);
  return;
}

using namespace mozilla;
using namespace mozilla::net;

#define ILLEGAL_FOLDER_CHARS ";#"
#define ILLEGAL_FOLDER_CHARS_AS_FIRST_LETTER "."
#define ILLEGAL_FOLDER_CHARS_AS_LAST_LETTER ".~ "

nsresult GetMessageServiceContractIDForURI(const char* uri,
                                           nsCString& contractID) {
  nsresult rv = NS_OK;
  // Find protocol
  nsAutoCString uriStr(uri);
  int32_t pos = uriStr.FindChar(':');
  if (pos == -1) return NS_ERROR_FAILURE;

  nsAutoCString protocol(StringHead(uriStr, pos));

  if (protocol.EqualsLiteral("file") &&
      uriStr.Find("application/x-message-display") != -1)
    protocol.AssignLiteral("mailbox");
  // Build message service contractid
  contractID = "@mozilla.org/messenger/messageservice;1?type=";
  contractID += protocol.get();

  return rv;
}

nsresult GetMessageServiceFromURI(const nsACString& uri,
                                  nsIMsgMessageService** aMessageService) {
  nsresult rv;

  nsAutoCString contractID;
  rv = GetMessageServiceContractIDForURI(PromiseFlatCString(uri).get(),
                                         contractID);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIMsgMessageService> msgService =
      do_GetService(contractID.get(), &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  msgService.forget(aMessageService);
  return rv;
}

nsresult GetMsgDBHdrFromURI(const nsACString& uri, nsIMsgDBHdr** msgHdr) {
  nsCOMPtr<nsIMsgMessageService> msgMessageService;
  nsresult rv =
      GetMessageServiceFromURI(uri, getter_AddRefs(msgMessageService));
  NS_ENSURE_SUCCESS(rv, rv);
  if (!msgMessageService) return NS_ERROR_FAILURE;

  return msgMessageService->MessageURIToMsgHdr(uri, msgHdr);
}

// Where should this live? It's a utility used to convert a string priority,
//  e.g., "High, Low, Normal" to an enum.
// Perhaps we should have an interface that groups together all these
//  utilities...
nsresult NS_MsgGetPriorityFromString(const char* const priority,
                                     nsMsgPriorityValue& outPriority) {
  if (!priority) return NS_ERROR_NULL_POINTER;

  // Note: Checking the values separately and _before_ the names,
  //        hoping for a much faster match;
  //       Only _drawback_, as "priority" handling is not truly specified:
  //        some software may have the number meanings reversed (1=Lowest) !?
  if (PL_strchr(priority, '1'))
    outPriority = nsMsgPriority::highest;
  else if (PL_strchr(priority, '2'))
    outPriority = nsMsgPriority::high;
  else if (PL_strchr(priority, '3'))
    outPriority = nsMsgPriority::normal;
  else if (PL_strchr(priority, '4'))
    outPriority = nsMsgPriority::low;
  else if (PL_strchr(priority, '5'))
    outPriority = nsMsgPriority::lowest;
  else if (PL_strcasestr(priority, "Highest"))
    outPriority = nsMsgPriority::highest;
  // Important: "High" must be tested after "Highest" !
  else if (PL_strcasestr(priority, "High") || PL_strcasestr(priority, "Urgent"))
    outPriority = nsMsgPriority::high;
  else if (PL_strcasestr(priority, "Normal"))
    outPriority = nsMsgPriority::normal;
  else if (PL_strcasestr(priority, "Lowest"))
    outPriority = nsMsgPriority::lowest;
  // Important: "Low" must be tested after "Lowest" !
  else if (PL_strcasestr(priority, "Low") ||
           PL_strcasestr(priority, "Non-urgent"))
    outPriority = nsMsgPriority::low;
  else
    // "Default" case gets default value.
    outPriority = nsMsgPriority::Default;

  return NS_OK;
}

nsresult NS_MsgGetPriorityValueString(const nsMsgPriorityValue p,
                                      nsACString& outValueString) {
  switch (p) {
    case nsMsgPriority::highest:
      outValueString.Assign('1');
      break;
    case nsMsgPriority::high:
      outValueString.Assign('2');
      break;
    case nsMsgPriority::normal:
      outValueString.Assign('3');
      break;
    case nsMsgPriority::low:
      outValueString.Assign('4');
      break;
    case nsMsgPriority::lowest:
      outValueString.Assign('5');
      break;
    case nsMsgPriority::none:
    case nsMsgPriority::notSet:
      // Note: '0' is a "fake" value; we expect to never be in this case.
      outValueString.Assign('0');
      break;
    default:
      NS_ASSERTION(false, "invalid priority value");
  }

  return NS_OK;
}

nsresult NS_MsgGetUntranslatedPriorityName(const nsMsgPriorityValue p,
                                           nsACString& outName) {
  switch (p) {
    case nsMsgPriority::highest:
      outName.AssignLiteral("Highest");
      break;
    case nsMsgPriority::high:
      outName.AssignLiteral("High");
      break;
    case nsMsgPriority::normal:
      outName.AssignLiteral("Normal");
      break;
    case nsMsgPriority::low:
      outName.AssignLiteral("Low");
      break;
    case nsMsgPriority::lowest:
      outName.AssignLiteral("Lowest");
      break;
    case nsMsgPriority::none:
    case nsMsgPriority::notSet:
      // Note: 'None' is a "fake" value; we expect to never be in this case.
      outName.AssignLiteral("None");
      break;
    default:
      NS_ASSERTION(false, "invalid priority value");
  }

  return NS_OK;
}

/* this used to be XP_StringHash2 from xp_hash.c */
/* phong's linear congruential hash  */
static uint32_t StringHash(const char* ubuf, int32_t len = -1) {
  unsigned char* buf = (unsigned char*)ubuf;
  uint32_t h = 1;
  unsigned char* end = buf + (len == -1 ? strlen(ubuf) : len);
  while (buf < end) {
    h = 0x63c63cd9 * h + 0x9c39c33d + (int32_t)*buf;
    buf++;
  }
  return h;
}

inline uint32_t StringHash(const nsAutoString& str) {
  const char16_t* strbuf = str.get();
  return StringHash(reinterpret_cast<const char*>(strbuf), str.Length() * 2);
}

/* Utility functions used in a few places in mailnews */
int32_t MsgFindCharInSet(const nsCString& aString, const char* aChars,
                         uint32_t aOffset) {
  return aString.FindCharInSet(aChars, aOffset);
}

int32_t MsgFindCharInSet(const nsString& aString, const char16_t* aChars,
                         uint32_t aOffset) {
  return aString.FindCharInSet(aChars, aOffset);
}

static bool ConvertibleToNative(const nsAutoString& str) {
  nsAutoCString native;
  nsAutoString roundTripped;
  NS_CopyUnicodeToNative(str, native);
  NS_CopyNativeToUnicode(native, roundTripped);
  return str.Equals(roundTripped);
}

#if defined(XP_UNIX)
const static uint32_t MAX_LEN = 55;
#elif defined(XP_WIN)
const static uint32_t MAX_LEN = 55;
#else
#  error need_to_define_your_max_filename_length
#endif

nsresult NS_MsgHashIfNecessary(nsAutoCString& name) {
  if (name.IsEmpty()) return NS_OK;  // Nothing to do.
  nsAutoCString str(name);

  // Given a filename, make it safe for filesystem
  // certain filenames require hashing because they
  // are too long or contain illegal characters
  int32_t illegalCharacterIndex = MsgFindCharInSet(
      str, FILE_PATH_SEPARATOR FILE_ILLEGAL_CHARACTERS ILLEGAL_FOLDER_CHARS, 0);

  // Need to check the first ('.') and last ('.', '~' and ' ') char
  if (illegalCharacterIndex == -1) {
    int32_t lastIndex = str.Length() - 1;
    if (nsLiteralCString(ILLEGAL_FOLDER_CHARS_AS_FIRST_LETTER)
            .FindChar(str[0]) != -1)
      illegalCharacterIndex = 0;
    else if (nsLiteralCString(ILLEGAL_FOLDER_CHARS_AS_LAST_LETTER)
                 .FindChar(str[lastIndex]) != -1)
      illegalCharacterIndex = lastIndex;
    else
      illegalCharacterIndex = -1;
  }

  char hashedname[MAX_LEN + 1];
  if (illegalCharacterIndex == -1) {
    // no illegal chars, it's just too long
    // keep the initial part of the string, but hash to make it fit
    if (str.Length() > MAX_LEN) {
      PL_strncpy(hashedname, str.get(), MAX_LEN + 1);
      PR_snprintf(hashedname + MAX_LEN - 8, 9, "%08lx",
                  (unsigned long)StringHash(str.get()));
      name = hashedname;
    }
  } else {
    // found illegal chars, hash the whole thing
    // if we do substitution, then hash, two strings
    // could hash to the same value.
    // for example, on mac:  "foo__bar", "foo:_bar", "foo::bar"
    // would map to "foo_bar".  this way, all three will map to
    // different values
    PR_snprintf(hashedname, 9, "%08lx", (unsigned long)StringHash(str.get()));
    name = hashedname;
  }

  return NS_OK;
}

// XXX : The number of UTF-16 2byte code units are half the number of
// bytes in legacy encodings for CJK strings and non-Latin1 in UTF-8.
// The ratio can be 1/3 for CJK strings in UTF-8. However, we can
// get away with using the same MAX_LEN for nsCString and nsString
// because MAX_LEN is defined rather conservatively in the first place.
nsresult NS_MsgHashIfNecessary(nsAutoString& name) {
  if (name.IsEmpty()) return NS_OK;  // Nothing to do.
  int32_t illegalCharacterIndex = MsgFindCharInSet(
      name,
      u"" FILE_PATH_SEPARATOR FILE_ILLEGAL_CHARACTERS ILLEGAL_FOLDER_CHARS, 0);

  // Need to check the first ('.') and last ('.', '~' and ' ') char
  if (illegalCharacterIndex == -1) {
    int32_t lastIndex = name.Length() - 1;
    if (NS_LITERAL_STRING_FROM_CSTRING(ILLEGAL_FOLDER_CHARS_AS_FIRST_LETTER)
            .FindChar(name[0]) != -1)
      illegalCharacterIndex = 0;
    else if (NS_LITERAL_STRING_FROM_CSTRING(ILLEGAL_FOLDER_CHARS_AS_LAST_LETTER)
                 .FindChar(name[lastIndex]) != -1)
      illegalCharacterIndex = lastIndex;
    else
      illegalCharacterIndex = -1;
  }

  char hashedname[9];
  int32_t keptLength = -1;
  if (illegalCharacterIndex != -1)
    keptLength = illegalCharacterIndex;
  else if (!ConvertibleToNative(name))
    keptLength = 0;
  else if (name.Length() > MAX_LEN) {
    keptLength = MAX_LEN - 8;
    // To avoid keeping only the high surrogate of a surrogate pair
    if (NS_IS_HIGH_SURROGATE(name.CharAt(keptLength - 1))) --keptLength;
  }

  if (keptLength >= 0) {
    PR_snprintf(hashedname, 9, "%08lx", (unsigned long)StringHash(name));
    name.SetLength(keptLength);
    name.Append(NS_ConvertASCIItoUTF16(hashedname));
  }

  return NS_OK;
}

nsresult FormatFileSize(int64_t size, bool useKB, nsAString& formattedSize) {
  const char* sizeAbbrNames[] = {
      "byteAbbreviation2",     "kiloByteAbbreviation2", "megaByteAbbreviation2",
      "gigaByteAbbreviation2", "teraByteAbbreviation2", "petaByteAbbreviation2",
  };

  nsresult rv;

  nsCOMPtr<nsIStringBundleService> bundleSvc =
      mozilla::components::StringBundle::Service();
  NS_ENSURE_TRUE(bundleSvc, NS_ERROR_UNEXPECTED);

  nsCOMPtr<nsIStringBundle> bundle;
  rv = bundleSvc->CreateBundle("chrome://messenger/locale/messenger.properties",
                               getter_AddRefs(bundle));
  NS_ENSURE_SUCCESS(rv, rv);

  double unitSize = size < 0 ? 0.0 : size;
  uint32_t unitIndex = 0;

  if (useKB) {
    // Start by formatting in kilobytes
    unitSize /= 1024;
    if (unitSize < 0.1 && unitSize != 0) unitSize = 0.1;
    unitIndex++;
  }

  // Convert to next unit if it needs 4 digits (after rounding), but only if
  // we know the name of the next unit
  while ((unitSize >= 999.5) && (unitIndex < ArrayLength(sizeAbbrNames) - 1)) {
    unitSize /= 1024;
    unitIndex++;
  }

  // Grab the string for the appropriate unit
  nsString sizeAbbr;
  rv = bundle->GetStringFromName(sizeAbbrNames[unitIndex], sizeAbbr);
  NS_ENSURE_SUCCESS(rv, rv);

  // Get rid of insignificant bits by truncating to 1 or 0 decimal points
  // 0.1 -> 0.1; 1.2 -> 1.2; 12.3 -> 12.3; 123.4 -> 123; 234.5 -> 235
  nsTextFormatter::ssprintf(
      formattedSize, sizeAbbr.get(),
      (unitIndex != 0) && (unitSize < 99.95 && unitSize != 0) ? 1 : 0,
      unitSize);

  int32_t separatorPos = formattedSize.FindChar('.');
  if (separatorPos != kNotFound) {
    // The ssprintf returned a decimal number using a dot (.) as the decimal
    // separator. Now we try to localize the separator.
    // Try to get the decimal separator from the system's locale.
    char* decimalPoint;
#ifdef HAVE_LOCALECONV
    struct lconv* locale = localeconv();
    decimalPoint = locale->decimal_point;
#else
    decimalPoint = getenv("LOCALE_DECIMAL_POINT");
#endif
    NS_ConvertUTF8toUTF16 decimalSeparator(decimalPoint);
    if (decimalSeparator.IsEmpty()) decimalSeparator.Assign('.');

    formattedSize.Replace(separatorPos, 1, decimalSeparator);
  }

  return NS_OK;
}

nsresult NS_MsgCreatePathStringFromFolderURI(const char* aFolderURI,
                                             nsCString& aPathCString,
                                             const nsCString& aScheme,
                                             bool aIsNewsFolder) {
  // A file name has to be in native charset. Here we convert
  // to UTF-16 and check for 'unsafe' characters before converting
  // to native charset.
  NS_ENSURE_TRUE(mozilla::IsUtf8(nsDependentCString(aFolderURI)),
                 NS_ERROR_UNEXPECTED);
  NS_ConvertUTF8toUTF16 oldPath(aFolderURI);

  nsAutoString pathPiece, path;

  int32_t startSlashPos = oldPath.FindChar('/');
  int32_t endSlashPos = (startSlashPos >= 0)
                            ? oldPath.FindChar('/', startSlashPos + 1) - 1
                            : oldPath.Length() - 1;
  if (endSlashPos < 0) endSlashPos = oldPath.Length();
#if defined(XP_UNIX) || defined(XP_MACOSX)
  bool isLocalUri = aScheme.EqualsLiteral("none") ||
                    aScheme.EqualsLiteral("pop3") ||
                    aScheme.EqualsLiteral("rss");
#endif
  // trick to make sure we only add the path to the first n-1 folders
  bool haveFirst = false;
  while (startSlashPos != -1) {
    pathPiece.Assign(
        Substring(oldPath, startSlashPos + 1, endSlashPos - startSlashPos));
    // skip leading '/' (and other // style things)
    if (!pathPiece.IsEmpty()) {
      // add .sbd onto the previous path
      if (haveFirst) {
        path.AppendLiteral(FOLDER_SUFFIX "/");
      }

      if (aIsNewsFolder) {
        nsAutoCString tmp;
        CopyUTF16toMUTF7(pathPiece, tmp);
        CopyASCIItoUTF16(tmp, pathPiece);
      }
#if defined(XP_UNIX) || defined(XP_MACOSX)
      // Don't hash path pieces because local mail folder uri's have already
      // been hashed. We're only doing this on the mac to limit potential
      // regressions.
      if (!isLocalUri)
#endif
        NS_MsgHashIfNecessary(pathPiece);
      path += pathPiece;
      haveFirst = true;
    }
    // look for the next slash
    startSlashPos = endSlashPos + 1;

    endSlashPos = (startSlashPos >= 0)
                      ? oldPath.FindChar('/', startSlashPos + 1) - 1
                      : oldPath.Length() - 1;
    if (endSlashPos < 0) endSlashPos = oldPath.Length();

    if (startSlashPos >= endSlashPos) break;
  }
  return NS_CopyUnicodeToNative(path, aPathCString);
}

bool NS_MsgStripRE(const nsCString& subject, nsCString& modifiedSubject) {
  bool result = false;

  // Get localizedRe pref.
  nsresult rv;
  nsString utf16LocalizedRe;
  NS_GetLocalizedUnicharPreferenceWithDefault(nullptr, "mailnews.localizedRe",
                                              EmptyString(), utf16LocalizedRe);
  NS_ConvertUTF16toUTF8 localizedRe(utf16LocalizedRe);

  // Hardcoded "Re" so that no one can configure Mozilla standards incompatible.
  nsAutoCString checkString("Re,RE,re,rE");
  if (!localizedRe.IsEmpty()) {
    checkString.Append(',');
    checkString.Append(localizedRe);
  }

  // Decode the string.
  nsCString decodedString;
  nsCOMPtr<nsIMimeConverter> mimeConverter;
  // We cannot strip "Re:" for RFC2047-encoded subject without modifying the
  // original.
  if (subject.Find("=?") != kNotFound) {
    mimeConverter = do_GetService(NS_MIME_CONVERTER_CONTRACTID, &rv);
    if (NS_SUCCEEDED(rv))
      rv = mimeConverter->DecodeMimeHeaderToUTF8(subject, nullptr, false, true,
                                                 decodedString);
  }

  const char *s, *s_end;
  if (decodedString.IsEmpty()) {
    s = subject.BeginReading();
    s_end = s + subject.Length();
  } else {
    s = decodedString.BeginReading();
    s_end = s + decodedString.Length();
  }

AGAIN:
  while (s < s_end && IS_SPACE(*s)) s++;

  const char* tokPtr = checkString.get();
  while (*tokPtr) {
    // Tokenize the comma separated list.
    size_t tokenLength = 0;
    while (*tokPtr && *tokPtr != ',') {
      tokenLength++;
      tokPtr++;
    }
    // Check if the beginning of s is the actual token.
    if (tokenLength && !strncmp(s, tokPtr - tokenLength, tokenLength)) {
      if (s[tokenLength] == ':') {
        s = s + tokenLength + 1; /* Skip over "Re:" */
        result = true;           /* Yes, we stripped it. */
        goto AGAIN;              /* Skip whitespace and try again. */
      } else if (s[tokenLength] == '[' || s[tokenLength] == '(') {
        const char* s2 = s + tokenLength + 1; /* Skip over "Re[" */

        // Skip forward over digits after the "[".
        while (s2 < (s_end - 2) && isdigit((unsigned char)*s2)) s2++;

        // Now ensure that the following thing is "]:".
        // Only if it is do we alter `s`.
        if ((s2[0] == ']' || s2[0] == ')') && s2[1] == ':') {
          s = s2 + 2;    /* Skip over "]:" */
          result = true; /* Yes, we stripped it. */
          goto AGAIN;    /* Skip whitespace and try again. */
        }
      }
    }
    if (*tokPtr) tokPtr++;
  }

  // If we didn't strip anything, we can return here.
  if (!result) return false;

  if (decodedString.IsEmpty()) {
    // We didn't decode anything, so just return a new string.
    modifiedSubject.Assign(s);
    return true;
  }

  // We decoded the string, so we need to encode it again. We always encode in
  // UTF-8.
  mimeConverter->EncodeMimePartIIStr_UTF8(
      nsDependentCString(s), false, sizeof("Subject:"),
      nsIMimeConverter::MIME_ENCODED_WORD_SIZE, modifiedSubject);
  return true;
}

/*  Very similar to strdup except it free's too
 */
char* NS_MsgSACopy(char** destination, const char* source) {
  if (*destination) {
    PR_Free(*destination);
    *destination = 0;
  }
  if (!source)
    *destination = nullptr;
  else {
    *destination = (char*)PR_Malloc(PL_strlen(source) + 1);
    if (*destination == nullptr) return (nullptr);

    PL_strcpy(*destination, source);
  }
  return *destination;
}

/*  Again like strdup but it concatenates and free's and uses Realloc.
 */
char* NS_MsgSACat(char** destination, const char* source) {
  if (source && *source) {
    int destLength = *destination ? PL_strlen(*destination) : 0;
    char* newDestination =
        (char*)PR_Realloc(*destination, destLength + PL_strlen(source) + 1);
    if (newDestination == nullptr) return nullptr;

    *destination = newDestination;
    PL_strcpy(*destination + destLength, source);
  }
  return *destination;
}

nsresult NS_MsgEscapeEncodeURLPath(const nsAString& aStr, nsCString& aResult) {
  return MsgEscapeString(NS_ConvertUTF16toUTF8(aStr),
                         nsINetUtil::ESCAPE_URL_PATH, aResult);
}

nsresult NS_MsgDecodeUnescapeURLPath(const nsACString& aPath,
                                     nsAString& aResult) {
  nsAutoCString unescapedName;
  MsgUnescapeString(
      aPath,
      nsINetUtil::ESCAPE_URL_FILE_BASENAME | nsINetUtil::ESCAPE_URL_FORCED,
      unescapedName);
  CopyUTF8toUTF16(unescapedName, aResult);
  return NS_OK;
}

bool WeAreOffline() {
  bool offline = false;

  nsCOMPtr<nsIIOService> ioService = mozilla::components::IO::Service();
  if (ioService) ioService->GetOffline(&offline);

  return offline;
}

// Find a folder by URL. If it doesn't exist, null will be returned
// via aFolder.
nsresult FindFolder(const nsACString& aFolderURI, nsIMsgFolder** aFolder) {
  NS_ENSURE_ARG_POINTER(aFolder);

  *aFolder = nullptr;

  nsresult rv;
  nsCOMPtr<nsIFolderLookupService> fls(do_GetService(NSIFLS_CONTRACTID, &rv));
  NS_ENSURE_SUCCESS(rv, rv);

  // GetFolderForURL returns NS_OK and null for non-existent folders
  rv = fls->GetFolderForURL(aFolderURI, aFolder);
  NS_ENSURE_SUCCESS(rv, rv);

  return NS_OK;
}

// Fetch an existing folder by URL
// The returned aFolder will be non-null if and only if result is NS_OK.
// NS_OK - folder was found
// NS_MSG_FOLDER_MISSING - if aFolderURI not found
nsresult GetExistingFolder(const nsACString& aFolderURI,
                           nsIMsgFolder** aFolder) {
  nsresult rv = FindFolder(aFolderURI, aFolder);
  NS_ENSURE_SUCCESS(rv, rv);
  return *aFolder ? NS_OK : NS_MSG_ERROR_FOLDER_MISSING;
}

nsresult GetOrCreateFolder(const nsACString& aFolderURI,
                           nsIMsgFolder** aFolder) {
  NS_ENSURE_ARG_POINTER(aFolder);

  *aFolder = nullptr;

  nsresult rv;
  nsCOMPtr<nsIFolderLookupService> fls(do_GetService(NSIFLS_CONTRACTID, &rv));
  NS_ENSURE_SUCCESS(rv, rv);

  rv = fls->GetOrCreateFolderForURL(aFolderURI, aFolder);
  NS_ENSURE_SUCCESS(rv, rv);

  return *aFolder ? NS_OK : NS_ERROR_FAILURE;
}

bool IsAFromSpaceLine(char* start, const char* end) {
  bool rv = false;
  while ((start < end) && (*start == '>')) start++;
  // If the leading '>'s are followed by an 'F' then we have a possible case
  // here.
  if ((*start == 'F') && (end - start > 4) && !strncmp(start, "From ", 5))
    rv = true;
  return rv;
}

//
// This function finds all lines starting with "From " or "From " preceding
// with one or more '>' (ie, ">From", ">>From", etc) in the input buffer
// (between 'start' and 'end') and prefix them with a ">" .
//
nsresult EscapeFromSpaceLine(nsIOutputStream* outputStream, char* start,
                             const char* end) {
  nsresult rv;
  char* pChar;
  uint32_t written;

  pChar = start;
  while (start < end) {
    while ((pChar < end) && (*pChar != '\r') && ((pChar + 1) < end) &&
           (*(pChar + 1) != '\n'))
      pChar++;
    if ((pChar + 1) == end) pChar++;

    if (pChar < end) {
      // Found a line so check if it's a qualified "From " line.
      if (IsAFromSpaceLine(start, pChar)) {
        rv = outputStream->Write(">", 1, &written);
        NS_ENSURE_SUCCESS(rv, rv);
      }
      int32_t lineTerminatorCount = (*(pChar + 1) == '\n') ? 2 : 1;
      rv = outputStream->Write(start, pChar - start + lineTerminatorCount,
                               &written);
      NS_ENSURE_SUCCESS(rv, rv);
      pChar += lineTerminatorCount;
      start = pChar;
    } else if (start < end) {
      // Check and flush out the remaining data and we're done.
      if (IsAFromSpaceLine(start, end)) {
        rv = outputStream->Write(">", 1, &written);
        NS_ENSURE_SUCCESS(rv, rv);
      }
      rv = outputStream->Write(start, end - start, &written);
      NS_ENSURE_SUCCESS(rv, rv);
      break;
    }
  }
  return NS_OK;
}

nsresult IsRFC822HeaderFieldName(const char* aHdr, bool* aResult) {
  NS_ENSURE_ARG_POINTER(aHdr);
  NS_ENSURE_ARG_POINTER(aResult);
  uint32_t length = strlen(aHdr);
  for (uint32_t i = 0; i < length; i++) {
    char c = aHdr[i];
    if (c < '!' || c == ':' || c > '~') {
      *aResult = false;
      return NS_OK;
    }
  }
  *aResult = true;
  return NS_OK;
}

// Warning, currently this routine only works for the Junk Folder
nsresult GetOrCreateJunkFolder(const nsACString& aURI,
                               nsIUrlListener* aListener) {
  nsresult rv;

  nsCOMPtr<nsIMsgFolder> folder;
  rv = GetOrCreateFolder(aURI, getter_AddRefs(folder));
  NS_ENSURE_SUCCESS(rv, rv);

  // don't check validity of folder - caller will handle creating it
  nsCOMPtr<nsIMsgIncomingServer> server;
  // make sure that folder hierarchy is built so that legitimate parent-child
  // relationship is established
  rv = folder->GetServer(getter_AddRefs(server));
  NS_ENSURE_SUCCESS(rv, rv);
  if (!server) return NS_ERROR_UNEXPECTED;

  nsCOMPtr<nsIMsgFolder> msgFolder;
  rv = server->GetMsgFolderFromURI(folder, aURI, getter_AddRefs(msgFolder));
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIMsgFolder> parent;
  rv = msgFolder->GetParent(getter_AddRefs(parent));
  if (NS_FAILED(rv) || !parent) {
    nsCOMPtr<nsIFile> folderPath;
    // for local folders, path is to the berkeley mailbox.
    // for imap folders, path needs to have .msf appended to the name
    msgFolder->GetFilePath(getter_AddRefs(folderPath));

    nsCOMPtr<nsIMsgProtocolInfo> protocolInfo;
    rv = server->GetProtocolInfo(getter_AddRefs(protocolInfo));
    NS_ENSURE_SUCCESS(rv, rv);

    bool isAsyncFolder;
    rv = protocolInfo->GetFoldersCreatedAsync(&isAsyncFolder);
    NS_ENSURE_SUCCESS(rv, rv);

    // if we can't get the path from the folder, then try to create the storage.
    // for imap, it doesn't matter if the .msf file exists - it still might not
    // exist on the server, so we should try to create it
    bool exists = false;
    if (!isAsyncFolder && folderPath) folderPath->Exists(&exists);
    if (!exists) {
      // Hack to work around a localization bug with the Junk Folder.
      // Please see Bug #270261 for more information...
      nsString localizedJunkName;
      msgFolder->GetName(localizedJunkName);

      // force the junk folder name to be Junk so it gets created on disk
      // correctly...
      msgFolder->SetName(u"Junk"_ns);
      msgFolder->SetFlag(nsMsgFolderFlags::Junk);
      rv = msgFolder->CreateStorageIfMissing(aListener);
      NS_ENSURE_SUCCESS(rv, rv);

      // now restore the localized folder name...
      msgFolder->SetName(localizedJunkName);

      // XXX TODO
      // JUNK MAIL RELATED
      // ugh, I hate this hack
      // we have to do this (for now)
      // because imap and local are different (one creates folder asynch, the
      // other synch) one will notify the listener, one will not. I blame
      // nsMsgCopy. we should look into making it so no matter what the folder
      // type we always call the listener this code should move into local
      // folder's version of CreateStorageIfMissing()
      if (!isAsyncFolder && aListener) {
        rv = aListener->OnStartRunningUrl(nullptr);
        NS_ENSURE_SUCCESS(rv, rv);

        rv = aListener->OnStopRunningUrl(nullptr, NS_OK);
        NS_ENSURE_SUCCESS(rv, rv);
      }
    }
  } else {
    // if the folder exists, we should set the junk flag on it
    // which is what the listener will do
    if (aListener) {
      rv = aListener->OnStartRunningUrl(nullptr);
      NS_ENSURE_SUCCESS(rv, rv);

      rv = aListener->OnStopRunningUrl(nullptr, NS_OK);
      NS_ENSURE_SUCCESS(rv, rv);
    }
  }

  return NS_OK;
}

nsresult IsRSSArticle(nsIURI* aMsgURI, bool* aIsRSSArticle) {
  nsresult rv;
  *aIsRSSArticle = false;

  nsCOMPtr<nsIMsgMessageUrl> msgUrl = do_QueryInterface(aMsgURI, &rv);
  if (NS_FAILED(rv)) return rv;

  nsCString resourceURI;
  msgUrl->GetUri(resourceURI);

  // get the msg service for this URI
  nsCOMPtr<nsIMsgMessageService> msgService;
  rv = GetMessageServiceFromURI(resourceURI, getter_AddRefs(msgService));
  NS_ENSURE_SUCCESS(rv, rv);

  // Check if the message is a feed message, regardless of folder.
  uint32_t flags;
  nsCOMPtr<nsIMsgDBHdr> msgHdr;
  rv = msgService->MessageURIToMsgHdr(resourceURI, getter_AddRefs(msgHdr));
  NS_ENSURE_SUCCESS(rv, rv);
  msgHdr->GetFlags(&flags);
  if (flags & nsMsgMessageFlags::FeedMsg) {
    *aIsRSSArticle = true;
    return rv;
  }

  nsCOMPtr<nsIMsgMailNewsUrl> mailnewsUrl = do_QueryInterface(aMsgURI, &rv);
  mozilla::Unused << mailnewsUrl;
  NS_ENSURE_SUCCESS(rv, rv);

  // get the folder and the server from the msghdr
  nsCOMPtr<nsIMsgFolder> folder;
  rv = msgHdr->GetFolder(getter_AddRefs(folder));
  if (NS_SUCCEEDED(rv) && folder) {
    nsCOMPtr<nsIMsgIncomingServer> server;
    folder->GetServer(getter_AddRefs(server));
    nsCOMPtr<nsIRssIncomingServer> rssServer = do_QueryInterface(server);

    if (rssServer) *aIsRSSArticle = true;
  }

  return rv;
}

// digest needs to be a pointer to a DIGEST_LENGTH (16) byte buffer
nsresult MSGCramMD5(const char* text, int32_t text_len, const char* key,
                    int32_t key_len, unsigned char* digest) {
  nsresult rv;

  nsAutoCString hash;
  nsCOMPtr<nsICryptoHash> hasher =
      do_CreateInstance("@mozilla.org/security/hash;1", &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  // this code adapted from
  // http://www.cis.ohio-state.edu/cgi-bin/rfc/rfc2104.html

  char innerPad[65]; /* inner padding - key XORd with innerPad */
  char outerPad[65]; /* outer padding - key XORd with outerPad */
  int i;
  /* if key is longer than 64 bytes reset it to key=MD5(key) */
  if (key_len > 64) {
    rv = hasher->Init(nsICryptoHash::MD5);
    NS_ENSURE_SUCCESS(rv, rv);

    rv = hasher->Update((const uint8_t*)key, key_len);
    NS_ENSURE_SUCCESS(rv, rv);

    rv = hasher->Finish(false, hash);
    NS_ENSURE_SUCCESS(rv, rv);

    key = hash.get();
    key_len = DIGEST_LENGTH;
  }

  /*
   * the HMAC_MD5 transform looks like:
   *
   * MD5(K XOR outerPad, MD5(K XOR innerPad, text))
   *
   * where K is an n byte key
   * innerPad is the byte 0x36 repeated 64 times
   * outerPad is the byte 0x5c repeated 64 times
   * and text is the data being protected
   */

  /* start out by storing key in pads */
  memset(innerPad, 0, sizeof innerPad);
  memset(outerPad, 0, sizeof outerPad);
  memcpy(innerPad, key, key_len);
  memcpy(outerPad, key, key_len);

  /* XOR key with innerPad and outerPad values */
  for (i = 0; i < 64; i++) {
    innerPad[i] ^= 0x36;
    outerPad[i] ^= 0x5c;
  }
  /*
   * perform inner MD5
   */
  nsAutoCString result;
  rv = hasher->Init(nsICryptoHash::MD5); /* init context for 1st pass */
  rv = hasher->Update((const uint8_t*)innerPad, 64); /* start with inner pad */
  rv = hasher->Update((const uint8_t*)text,
                      text_len);      /* then text of datagram */
  rv = hasher->Finish(false, result); /* finish up 1st pass */

  /*
   * perform outer MD5
   */
  hasher->Init(nsICryptoHash::MD5); /* init context for 2nd pass */
  rv = hasher->Update((const uint8_t*)outerPad, 64); /* start with outer pad */
  rv = hasher->Update((const uint8_t*)result.get(),
                      16);            /* then results of 1st hash */
  rv = hasher->Finish(false, result); /* finish up 2nd pass */

  if (result.Length() != DIGEST_LENGTH) return NS_ERROR_UNEXPECTED;

  memcpy(digest, result.get(), DIGEST_LENGTH);

  return rv;
}

// digest needs to be a pointer to a DIGEST_LENGTH (16) byte buffer
nsresult MSGApopMD5(const char* text, int32_t text_len, const char* password,
                    int32_t password_len, unsigned char* digest) {
  nsresult rv;
  nsAutoCString result;

  nsCOMPtr<nsICryptoHash> hasher =
      do_CreateInstance("@mozilla.org/security/hash;1", &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = hasher->Init(nsICryptoHash::MD5);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = hasher->Update((const uint8_t*)text, text_len);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = hasher->Update((const uint8_t*)password, password_len);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = hasher->Finish(false, result);
  NS_ENSURE_SUCCESS(rv, rv);

  if (result.Length() != DIGEST_LENGTH) return NS_ERROR_UNEXPECTED;

  memcpy(digest, result.get(), DIGEST_LENGTH);
  return rv;
}

NS_MSG_BASE nsresult NS_GetPersistentFile(const char* relPrefName,
                                          const char* absPrefName,
                                          const char* dirServiceProp,
                                          bool& gotRelPref, nsIFile** aFile,
                                          nsIPrefBranch* prefBranch) {
  NS_ENSURE_ARG_POINTER(aFile);
  *aFile = nullptr;
  NS_ENSURE_ARG(relPrefName);
  NS_ENSURE_ARG(absPrefName);
  gotRelPref = false;

  nsCOMPtr<nsIPrefBranch> mainBranch;
  if (!prefBranch) {
    nsCOMPtr<nsIPrefService> prefService(
        do_GetService(NS_PREFSERVICE_CONTRACTID));
    if (!prefService) return NS_ERROR_FAILURE;
    prefService->GetBranch(nullptr, getter_AddRefs(mainBranch));
    if (!mainBranch) return NS_ERROR_FAILURE;
    prefBranch = mainBranch;
  }

  nsCOMPtr<nsIFile> localFile;

  // Get the relative first
  nsCOMPtr<nsIRelativeFilePref> relFilePref;
  prefBranch->GetComplexValue(relPrefName, NS_GET_IID(nsIRelativeFilePref),
                              getter_AddRefs(relFilePref));
  if (relFilePref) {
    relFilePref->GetFile(getter_AddRefs(localFile));
    NS_ASSERTION(localFile, "An nsIRelativeFilePref has no file.");
    if (localFile) gotRelPref = true;
  }

  // If not, get the old absolute
  if (!localFile) {
    prefBranch->GetComplexValue(absPrefName, NS_GET_IID(nsIFile),
                                getter_AddRefs(localFile));

    // If not, and given a dirServiceProp, use directory service.
    if (!localFile && dirServiceProp) {
      nsCOMPtr<nsIProperties> dirService(
          do_GetService("@mozilla.org/file/directory_service;1"));
      if (!dirService) return NS_ERROR_FAILURE;
      dirService->Get(dirServiceProp, NS_GET_IID(nsIFile),
                      getter_AddRefs(localFile));
      if (!localFile) return NS_ERROR_FAILURE;
    }
  }

  if (localFile) {
    localFile->Normalize();
    localFile.forget(aFile);
    return NS_OK;
  }

  return NS_ERROR_FAILURE;
}

NS_MSG_BASE nsresult NS_SetPersistentFile(const char* relPrefName,
                                          const char* absPrefName,
                                          nsIFile* aFile,
                                          nsIPrefBranch* prefBranch) {
  NS_ENSURE_ARG(relPrefName);
  NS_ENSURE_ARG(absPrefName);
  NS_ENSURE_ARG(aFile);

  nsCOMPtr<nsIPrefBranch> mainBranch;
  if (!prefBranch) {
    nsCOMPtr<nsIPrefService> prefService(
        do_GetService(NS_PREFSERVICE_CONTRACTID));
    if (!prefService) return NS_ERROR_FAILURE;
    prefService->GetBranch(nullptr, getter_AddRefs(mainBranch));
    if (!mainBranch) return NS_ERROR_FAILURE;
    prefBranch = mainBranch;
  }

  // Write the absolute for backwards compatibilty's sake.
  // Or, if aPath is on a different drive than the profile dir.
  nsresult rv =
      prefBranch->SetComplexValue(absPrefName, NS_GET_IID(nsIFile), aFile);

  // Write the relative path.
  nsCOMPtr<nsIRelativeFilePref> relFilePref = new nsRelativeFilePref();
  mozilla::Unused << relFilePref->SetFile(aFile);
  mozilla::Unused << relFilePref->SetRelativeToKey(
      nsLiteralCString(NS_APP_USER_PROFILE_50_DIR));

  nsresult rv2 = prefBranch->SetComplexValue(
      relPrefName, NS_GET_IID(nsIRelativeFilePref), relFilePref);
  if (NS_FAILED(rv2) && NS_SUCCEEDED(rv))
    prefBranch->ClearUserPref(relPrefName);

  return rv;
}

NS_MSG_BASE nsresult NS_GetUnicharPreferenceWithDefault(
    nsIPrefBranch* prefBranch,  // can be null, if so uses the root branch
    const char* prefName, const nsAString& defValue, nsAString& prefValue) {
  NS_ENSURE_ARG(prefName);

  nsCOMPtr<nsIPrefBranch> pbr;
  if (!prefBranch) {
    pbr = do_GetService(NS_PREFSERVICE_CONTRACTID);
    prefBranch = pbr;
  }

  nsCString valueUtf8;
  nsresult rv =
      prefBranch->GetStringPref(prefName, EmptyCString(), 0, valueUtf8);
  if (NS_SUCCEEDED(rv))
    CopyUTF8toUTF16(valueUtf8, prefValue);
  else
    prefValue = defValue;
  return NS_OK;
}

NS_MSG_BASE nsresult NS_GetLocalizedUnicharPreferenceWithDefault(
    nsIPrefBranch* prefBranch,  // can be null, if so uses the root branch
    const char* prefName, const nsAString& defValue, nsAString& prefValue) {
  NS_ENSURE_ARG(prefName);

  nsCOMPtr<nsIPrefBranch> pbr;
  if (!prefBranch) {
    pbr = do_GetService(NS_PREFSERVICE_CONTRACTID);
    prefBranch = pbr;
  }

  nsCOMPtr<nsIPrefLocalizedString> str;
  nsresult rv = prefBranch->GetComplexValue(
      prefName, NS_GET_IID(nsIPrefLocalizedString), getter_AddRefs(str));
  if (NS_SUCCEEDED(rv)) {
    nsString tmpValue;
    str->ToString(getter_Copies(tmpValue));
    prefValue.Assign(tmpValue);
  } else
    prefValue = defValue;
  return NS_OK;
}

NS_MSG_BASE nsresult NS_GetLocalizedUnicharPreference(
    nsIPrefBranch* prefBranch,  // can be null, if so uses the root branch
    const char* prefName, nsAString& prefValue) {
  NS_ENSURE_ARG_POINTER(prefName);

  nsCOMPtr<nsIPrefBranch> pbr;
  if (!prefBranch) {
    pbr = do_GetService(NS_PREFSERVICE_CONTRACTID);
    prefBranch = pbr;
  }

  nsCOMPtr<nsIPrefLocalizedString> str;
  nsresult rv = prefBranch->GetComplexValue(
      prefName, NS_GET_IID(nsIPrefLocalizedString), getter_AddRefs(str));
  NS_ENSURE_SUCCESS(rv, rv);

  nsString tmpValue;
  str->ToString(getter_Copies(tmpValue));
  prefValue.Assign(tmpValue);
  return NS_OK;
}

void PRTime2Seconds(PRTime prTime, uint32_t* seconds) {
  *seconds = (uint32_t)(prTime / PR_USEC_PER_SEC);
}

void PRTime2Seconds(PRTime prTime, int32_t* seconds) {
  *seconds = (int32_t)(prTime / PR_USEC_PER_SEC);
}

void Seconds2PRTime(uint32_t seconds, PRTime* prTime) {
  *prTime = (PRTime)seconds * PR_USEC_PER_SEC;
}

nsresult GetSummaryFileLocation(nsIFile* fileLocation,
                                nsIFile** summaryLocation) {
  nsresult rv;
  nsCOMPtr<nsIFile> newSummaryLocation =
      do_CreateInstance(NS_LOCAL_FILE_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  newSummaryLocation->InitWithFile(fileLocation);
  nsString fileName;

  rv = newSummaryLocation->GetLeafName(fileName);
  if (NS_FAILED(rv)) return rv;

  fileName.AppendLiteral(SUMMARY_SUFFIX);
  rv = newSummaryLocation->SetLeafName(fileName);
  NS_ENSURE_SUCCESS(rv, rv);

  newSummaryLocation.forget(summaryLocation);
  return NS_OK;
}

void MsgGenerateNowStr(nsACString& nowStr) {
  char dateBuf[100];
  dateBuf[0] = '\0';
  PRExplodedTime exploded;
  PR_ExplodeTime(PR_Now(), PR_LocalTimeParameters, &exploded);
  PR_FormatTimeUSEnglish(dateBuf, sizeof(dateBuf), "%a %b %d %H:%M:%S %Y",
                         &exploded);
  nowStr.Assign(dateBuf);
}

// Gets a special directory and appends the supplied file name onto it.
nsresult GetSpecialDirectoryWithFileName(const char* specialDirName,
                                         const char* fileName,
                                         nsIFile** result) {
  nsresult rv = NS_GetSpecialDirectory(specialDirName, result);
  NS_ENSURE_SUCCESS(rv, rv);

  return (*result)->AppendNative(nsDependentCString(fileName));
}

// Cleans up temp files with matching names
nsresult MsgCleanupTempFiles(const char* fileName, const char* extension) {
  nsCOMPtr<nsIFile> tmpFile;
  nsCString rootName(fileName);
  rootName.Append('.');
  rootName.Append(extension);
  nsresult rv = GetSpecialDirectoryWithFileName(NS_OS_TEMP_DIR, rootName.get(),
                                                getter_AddRefs(tmpFile));

  NS_ENSURE_SUCCESS(rv, rv);
  int index = 1;
  bool exists;
  do {
    tmpFile->Exists(&exists);
    if (exists) {
      tmpFile->Remove(false);
      nsCString leafName(fileName);
      leafName.Append('-');
      leafName.AppendInt(index);
      leafName.Append('.');
      leafName.Append(extension);
      // start with "Picture-1.jpg" after "Picture.jpg" exists
      tmpFile->SetNativeLeafName(leafName);
    }
  } while (exists && ++index < 10000);
  return NS_OK;
}

nsresult MsgGetFileStream(nsIFile* file, nsIOutputStream** fileStream) {
  RefPtr<nsMsgFileStream> newFileStream = new nsMsgFileStream;
  nsresult rv = newFileStream->InitWithFile(file);
  NS_ENSURE_SUCCESS(rv, rv);
  newFileStream.forget(fileStream);
  return NS_OK;
}

nsresult MsgNewBufferedFileOutputStream(nsIOutputStream** aResult,
                                        nsIFile* aFile, int32_t aIOFlags,
                                        int32_t aPerm) {
  nsCOMPtr<nsIOutputStream> stream;
  nsresult rv = NS_NewLocalFileOutputStream(getter_AddRefs(stream), aFile,
                                            aIOFlags, aPerm);
  if (NS_SUCCEEDED(rv))
    rv = NS_NewBufferedOutputStream(aResult, stream.forget(),
                                    FILE_IO_BUFFER_SIZE);
  return rv;
}

nsresult MsgNewSafeBufferedFileOutputStream(nsIOutputStream** aResult,
                                            nsIFile* aFile, int32_t aIOFlags,
                                            int32_t aPerm) {
  nsCOMPtr<nsIOutputStream> stream;
  nsresult rv = NS_NewSafeLocalFileOutputStream(getter_AddRefs(stream), aFile,
                                                aIOFlags, aPerm);
  if (NS_SUCCEEDED(rv))
    rv = NS_NewBufferedOutputStream(aResult, stream.forget(),
                                    FILE_IO_BUFFER_SIZE);
  return rv;
}

bool MsgFindKeyword(const nsCString& keyword, nsCString& keywords,
                    int32_t* aStartOfKeyword, int32_t* aLength) {
// nsTString_CharT::Find(const nsCString& aString,
//                       bool aIgnoreCase=false,
//                       int32_t aOffset=0,
//                       int32_t aCount=-1 ) const;
#define FIND_KEYWORD(keywords, keyword, offset) \
  ((keywords).Find((keyword), (offset)))
  // 'keyword' is the single keyword we're looking for
  // 'keywords' is a space delimited list of keywords to be searched,
  // which may be just a single keyword or even be empty
  const int32_t kKeywordLen = keyword.Length();
  const char* start = keywords.BeginReading();
  const char* end = keywords.EndReading();
  *aStartOfKeyword = FIND_KEYWORD(keywords, keyword, 0);
  while (*aStartOfKeyword >= 0) {
    const char* matchStart = start + *aStartOfKeyword;
    const char* matchEnd = matchStart + kKeywordLen;
    // For a real match, matchStart must be the start of keywords or preceded
    // by a space and matchEnd must be the end of keywords or point to a space.
    if ((matchStart == start || *(matchStart - 1) == ' ') &&
        (matchEnd == end || *matchEnd == ' ')) {
      *aLength = kKeywordLen;
      return true;
    }
    *aStartOfKeyword =
        FIND_KEYWORD(keywords, keyword, *aStartOfKeyword + kKeywordLen);
  }

  *aLength = 0;
  return false;
#undef FIND_KEYWORD
}

bool MsgHostDomainIsTrusted(nsCString& host, nsCString& trustedMailDomains) {
  const char* end;
  uint32_t hostLen, domainLen;
  bool domainIsTrusted = false;

  const char* domain = trustedMailDomains.BeginReading();
  const char* domainEnd = trustedMailDomains.EndReading();
  const char* hostStart = host.BeginReading();
  hostLen = host.Length();

  do {
    // skip any whitespace
    while (*domain == ' ' || *domain == '\t') ++domain;

    // find end of this domain in the string
    end = strchr(domain, ',');
    if (!end) end = domainEnd;

    // to see if the hostname is in the domain, check if the domain
    // matches the end of the hostname.
    domainLen = end - domain;
    if (domainLen && hostLen >= domainLen) {
      const char* hostTail = hostStart + hostLen - domainLen;
      if (PL_strncasecmp(domain, hostTail, domainLen) == 0) {
        // now, make sure either that the hostname is a direct match or
        // that the hostname begins with a dot.
        if (hostLen == domainLen || *hostTail == '.' ||
            *(hostTail - 1) == '.') {
          domainIsTrusted = true;
          break;
        }
      }
    }

    domain = end + 1;
  } while (*end);
  return domainIsTrusted;
}

nsresult MsgGetLocalFileFromURI(const nsACString& aUTF8Path, nsIFile** aFile) {
  nsresult rv;
  nsCOMPtr<nsIURI> argURI;
  rv = NS_NewURI(getter_AddRefs(argURI), aUTF8Path);
  NS_ENSURE_SUCCESS(rv, rv);
  nsCOMPtr<nsIFileURL> argFileURL(do_QueryInterface(argURI, &rv));
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIFile> argFile;
  rv = argFileURL->GetFile(getter_AddRefs(argFile));
  NS_ENSURE_SUCCESS(rv, rv);

  argFile.forget(aFile);
  return NS_OK;
}

NS_MSG_BASE void MsgStripQuotedPrintable(nsCString& aSrc) {
  // decode quoted printable text in place

  if (aSrc.IsEmpty()) return;

  char* src = aSrc.BeginWriting();
  char* dest = src;
  int srcIdx = 0, destIdx = 0;

  while (src[srcIdx] != 0) {
    // Decode sequence of '=XY' into a character with code XY.
    if (src[srcIdx] == '=') {
      if (MsgIsHex((const char*)src + srcIdx + 1, 2)) {
        // If we got here, we successfully decoded a quoted printable sequence,
        // so bump each pointer past it and move on to the next char.
        dest[destIdx++] = MsgUnhex((const char*)src + srcIdx + 1, 2);
        srcIdx += 3;
      } else {
        // If first char after '=' isn't hex check if it's a normal char
        // or a soft line break. If it's a soft line break, eat the
        // CR/LF/CRLF.
        if (src[srcIdx + 1] == '\r' || src[srcIdx + 1] == '\n') {
          srcIdx++;  // soft line break, ignore the '=';
          if (src[srcIdx] == '\r' || src[srcIdx] == '\n') {
            srcIdx++;
            if (src[srcIdx] == '\n') srcIdx++;
          }
        } else  // The first or second char after '=' isn't hex, just copy the
                // '='.
        {
          dest[destIdx++] = src[srcIdx++];
        }
        continue;
      }
    } else
      dest[destIdx++] = src[srcIdx++];
  }

  dest[destIdx] = src[srcIdx];  // null terminate
  aSrc.SetLength(destIdx);
}

NS_MSG_BASE nsresult MsgEscapeString(const nsACString& aStr, uint32_t aType,
                                     nsACString& aResult) {
  nsresult rv;
  nsCOMPtr<nsINetUtil> nu = do_GetService(NS_NETUTIL_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  return nu->EscapeString(aStr, aType, aResult);
}

NS_MSG_BASE nsresult MsgUnescapeString(const nsACString& aStr, uint32_t aFlags,
                                       nsACString& aResult) {
  nsresult rv;
  nsCOMPtr<nsINetUtil> nu = do_GetService(NS_NETUTIL_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  return nu->UnescapeString(aStr, aFlags, aResult);
}

NS_MSG_BASE nsresult MsgEscapeURL(const nsACString& aStr, uint32_t aFlags,
                                  nsACString& aResult) {
  nsresult rv;
  nsCOMPtr<nsINetUtil> nu = do_GetService(NS_NETUTIL_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  return nu->EscapeURL(aStr, aFlags, aResult);
}

NS_MSG_BASE nsresult
MsgGetHeadersFromKeys(nsIMsgDatabase* aDB, const nsTArray<nsMsgKey>& aMsgKeys,
                      nsTArray<RefPtr<nsIMsgDBHdr>>& aHeaders) {
  NS_ENSURE_ARG_POINTER(aDB);
  aHeaders.Clear();
  aHeaders.SetCapacity(aMsgKeys.Length());

  for (auto key : aMsgKeys) {
    // This function silently skips when the key is not found. This is an
    // expected case.
    bool hasKey;
    nsresult rv = aDB->ContainsKey(key, &hasKey);
    NS_ENSURE_SUCCESS(rv, rv);
    if (hasKey) {
      nsCOMPtr<nsIMsgDBHdr> msgHdr;
      rv = aDB->GetMsgHdrForKey(key, getter_AddRefs(msgHdr));
      NS_ENSURE_SUCCESS(rv, rv);
      aHeaders.AppendElement(msgHdr);
    }
  }
  return NS_OK;
}

bool MsgAdvanceToNextLine(const char* buffer, uint32_t& bufferOffset,
                          uint32_t maxBufferOffset) {
  bool result = false;
  for (; bufferOffset < maxBufferOffset; bufferOffset++) {
    if (buffer[bufferOffset] == '\r' || buffer[bufferOffset] == '\n') {
      bufferOffset++;
      if (buffer[bufferOffset - 1] == '\r' && buffer[bufferOffset] == '\n')
        bufferOffset++;
      result = true;
      break;
    }
  }
  return result;
}

NS_MSG_BASE nsresult MsgExamineForProxyAsync(nsIChannel* channel,
                                             nsIProtocolProxyCallback* listener,
                                             nsICancelable** result) {
  nsresult rv;

#ifdef DEBUG
  nsCOMPtr<nsIURI> uri;
  rv = channel->GetURI(getter_AddRefs(uri));
  NS_ASSERTION(NS_SUCCEEDED(rv) && uri,
               "The URI needs to be set before calling the proxy service");
#endif

  nsCOMPtr<nsIProtocolProxyService> pps =
      do_GetService(NS_PROTOCOLPROXYSERVICE_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  return pps->AsyncResolve(channel, 0, listener, nullptr, result);
}

NS_MSG_BASE nsresult MsgPromptLoginFailed(nsIMsgWindow* aMsgWindow,
                                          const nsACString& aHostname,
                                          const nsACString& aUsername,
                                          const nsAString& aAccountname,
                                          int32_t* aResult) {
  nsCOMPtr<nsIPrompt> dialog;
  if (aMsgWindow) aMsgWindow->GetPromptDialog(getter_AddRefs(dialog));

  nsresult rv;

  // If we haven't got one, use a default dialog.
  if (!dialog) {
    nsCOMPtr<nsIWindowWatcher> wwatch =
        do_GetService(NS_WINDOWWATCHER_CONTRACTID, &rv);
    NS_ENSURE_SUCCESS(rv, rv);

    rv = wwatch->GetNewPrompter(0, getter_AddRefs(dialog));
    NS_ENSURE_SUCCESS(rv, rv);
  }

  nsCOMPtr<nsIStringBundleService> bundleSvc =
      mozilla::components::StringBundle::Service();
  NS_ENSURE_TRUE(bundleSvc, NS_ERROR_UNEXPECTED);

  nsCOMPtr<nsIStringBundle> bundle;
  rv = bundleSvc->CreateBundle("chrome://messenger/locale/messenger.properties",
                               getter_AddRefs(bundle));
  NS_ENSURE_SUCCESS(rv, rv);

  nsString message;
  AutoTArray<nsString, 2> formatStrings;
  CopyUTF8toUTF16(aHostname, *formatStrings.AppendElement());
  CopyUTF8toUTF16(aUsername, *formatStrings.AppendElement());

  rv = bundle->FormatStringFromName("mailServerLoginFailed2", formatStrings,
                                    message);
  NS_ENSURE_SUCCESS(rv, rv);

  nsString title;
  if (aAccountname.IsEmpty()) {
    // Account name may be empty e.g. on a SMTP server.
    rv = bundle->GetStringFromName("mailServerLoginFailedTitle", title);
  } else {
    AutoTArray<nsString, 1> formatStrings = {nsString(aAccountname)};
    rv = bundle->FormatStringFromName("mailServerLoginFailedTitleWithAccount",
                                      formatStrings, title);
  }
  NS_ENSURE_SUCCESS(rv, rv);

  nsString button0;
  rv = bundle->GetStringFromName("mailServerLoginFailedRetryButton", button0);
  NS_ENSURE_SUCCESS(rv, rv);

  nsString button2;
  rv = bundle->GetStringFromName("mailServerLoginFailedEnterNewPasswordButton",
                                 button2);
  NS_ENSURE_SUCCESS(rv, rv);

  bool dummyValue = false;
  return dialog->ConfirmEx(
      title.get(), message.get(),
      (nsIPrompt::BUTTON_TITLE_IS_STRING * nsIPrompt::BUTTON_POS_0) +
          (nsIPrompt::BUTTON_TITLE_CANCEL * nsIPrompt::BUTTON_POS_1) +
          (nsIPrompt::BUTTON_TITLE_IS_STRING * nsIPrompt::BUTTON_POS_2),
      button0.get(), nullptr, button2.get(), nullptr, &dummyValue, aResult);
}

NS_MSG_BASE PRTime MsgConvertAgeInDaysToCutoffDate(int32_t ageInDays) {
  PRTime now = PR_Now();

  return now - PR_USEC_PER_DAY * ageInDays;
}

NS_MSG_BASE nsresult
MsgTermListToString(nsTArray<RefPtr<nsIMsgSearchTerm>> const& aTermList,
                    nsCString& aOutString) {
  nsresult rv = NS_OK;
  for (nsIMsgSearchTerm* term : aTermList) {
    nsAutoCString stream;

    if (aOutString.Length() > 1) aOutString += ' ';

    bool booleanAnd;
    bool matchAll;
    term->GetBooleanAnd(&booleanAnd);
    term->GetMatchAll(&matchAll);
    if (matchAll) {
      aOutString += "ALL";
      continue;
    } else if (booleanAnd)
      aOutString += "AND (";
    else
      aOutString += "OR (";

    rv = term->GetTermAsString(stream);
    NS_ENSURE_SUCCESS(rv, rv);

    aOutString += stream;
    aOutString += ')';
  }
  return rv;
}

NS_MSG_BASE uint64_t ParseUint64Str(const char* str) {
#ifdef XP_WIN
  {
    char* endPtr;
    return _strtoui64(str, &endPtr, 10);
  }
#else
  return strtoull(str, nullptr, 10);
#endif
}

NS_MSG_BASE uint64_t MsgUnhex(const char* aHexString, size_t aNumChars) {
  // Large numbers will not fit into uint64_t.
  NS_ASSERTION(aNumChars <= 16, "Hex literal too long to convert!");

  uint64_t result = 0;
  for (size_t i = 0; i < aNumChars; i++) {
    unsigned char c = aHexString[i];
    uint8_t digit;
    if ((c >= '0') && (c <= '9'))
      digit = (c - '0');
    else if ((c >= 'a') && (c <= 'f'))
      digit = ((c - 'a') + 10);
    else if ((c >= 'A') && (c <= 'F'))
      digit = ((c - 'A') + 10);
    else
      break;

    result = (result << 4) | digit;
  }

  return result;
}

NS_MSG_BASE bool MsgIsHex(const char* aHexString, size_t aNumChars) {
  for (size_t i = 0; i < aNumChars; i++) {
    if (!isxdigit(aHexString[i])) return false;
  }
  return true;
}

NS_MSG_BASE nsresult MsgStreamMsgHeaders(nsIInputStream* aInputStream,
                                         nsIStreamListener* aConsumer) {
  mozilla::UniquePtr<nsLineBuffer<char>> lineBuffer(new nsLineBuffer<char>);

  nsresult rv;

  nsAutoCString msgHeaders;
  nsAutoCString curLine;

  bool more = true;

  // We want to NS_ReadLine until we get to a blank line (the end of the
  // headers)
  while (more) {
    rv = NS_ReadLine(aInputStream, lineBuffer.get(), curLine, &more);
    NS_ENSURE_SUCCESS(rv, rv);
    if (curLine.IsEmpty()) break;
    msgHeaders.Append(curLine);
    msgHeaders.AppendLiteral("\r\n");
  }
  lineBuffer.reset();
  nsCOMPtr<nsIStringInputStream> hdrsStream =
      do_CreateInstance("@mozilla.org/io/string-input-stream;1", &rv);
  NS_ENSURE_SUCCESS(rv, rv);
  hdrsStream->SetData(msgHeaders.get(), msgHeaders.Length());

  nsCOMPtr<nsIInputStreamPump> pump;
  rv = NS_NewInputStreamPump(getter_AddRefs(pump), hdrsStream.forget());
  NS_ENSURE_SUCCESS(rv, rv);

  return pump->AsyncRead(aConsumer);
}

NS_MSG_BASE nsresult MsgDetectCharsetFromFile(nsIFile* aFile,
                                              nsACString& aCharset) {
  // We do the detection in this order:
  // Check BOM.
  // If no BOM, run localized detection (Russian, Ukrainian or Japanese).
  // We need to run this first, since ISO-2022-JP is 7bit ASCII and would be
  // detected as UTF-8. If ISO-2022-JP not detected, check for UTF-8. If no
  // UTF-8, but detector detected something, use that, otherwise return an
  // error.
  aCharset.Truncate();

  nsresult rv;
  nsCOMPtr<nsIInputStream> inputStream;
  rv = NS_NewLocalFileInputStream(getter_AddRefs(inputStream), aFile);
  NS_ENSURE_SUCCESS(rv, rv);

  // Check the BOM.
  char sniffBuf[3];
  uint32_t numRead;
  rv = inputStream->Read(sniffBuf, sizeof(sniffBuf), &numRead);

  if (numRead >= 2 && sniffBuf[0] == (char)0xfe && sniffBuf[1] == (char)0xff) {
    aCharset = "UTF-16BE";
  } else if (numRead >= 2 && sniffBuf[0] == (char)0xff &&
             sniffBuf[1] == (char)0xfe) {
    aCharset = "UTF-16LE";
  } else if (numRead >= 3 && sniffBuf[0] == (char)0xef &&
             sniffBuf[1] == (char)0xbb && sniffBuf[2] == (char)0xbf) {
    aCharset = "UTF-8";
  }
  if (!aCharset.IsEmpty()) return NS_OK;

  // Position back to the beginning.
  nsCOMPtr<nsISeekableStream> seekStream = do_QueryInterface(inputStream);
  if (seekStream) seekStream->Seek(nsISeekableStream::NS_SEEK_SET, 0);

  // Use detector.
  mozilla::UniquePtr<mozilla::EncodingDetector> detector =
      mozilla::EncodingDetector::Create();
  char buffer[1024];
  numRead = 0;
  while (NS_SUCCEEDED(inputStream->Read(buffer, sizeof(buffer), &numRead))) {
    mozilla::Span<const uint8_t> src =
        mozilla::AsBytes(mozilla::Span(buffer, numRead));
    Unused << detector->Feed(src, false);
    if (numRead == 0) {
      break;
    }
  }
  Unused << detector->Feed(nullptr, true);
  auto encoding = detector->Guess(nullptr, true);
  encoding->Name(aCharset);
  return NS_OK;
}

/*
 * Converts a buffer to plain text. Some conversions may
 * or may not work with certain end charsets which is why we
 * need that as an argument to the function. If charset is
 * unknown or deemed of no importance NULL could be passed.
 */
NS_MSG_BASE nsresult ConvertBufToPlainText(nsString& aConBuf, bool formatFlowed,
                                           bool formatOutput,
                                           bool disallowBreaks) {
  if (aConBuf.IsEmpty()) return NS_OK;

  int32_t wrapWidth = 72;
  nsCOMPtr<nsIPrefBranch> pPrefBranch(do_GetService(NS_PREFSERVICE_CONTRACTID));

  if (pPrefBranch) {
    pPrefBranch->GetIntPref("mailnews.wraplength", &wrapWidth);
    // Let sanity reign!
    if (wrapWidth == 0 || wrapWidth > 990)
      wrapWidth = 990;
    else if (wrapWidth < 10)
      wrapWidth = 10;
  }

  uint32_t converterFlags = nsIDocumentEncoder::OutputPersistNBSP;
  if (formatFlowed) converterFlags |= nsIDocumentEncoder::OutputFormatFlowed;
  if (formatOutput) converterFlags |= nsIDocumentEncoder::OutputFormatted;
  if (disallowBreaks)
    converterFlags |= nsIDocumentEncoder::OutputDisallowLineBreaking;

  nsCOMPtr<nsIParserUtils> utils = do_GetService(NS_PARSERUTILS_CONTRACTID);
  return utils->ConvertToPlainText(aConBuf, converterFlags, wrapWidth, aConBuf);
}

NS_MSG_BASE nsMsgKey msgKeyFromInt(uint32_t aValue) { return aValue; }

NS_MSG_BASE nsMsgKey msgKeyFromInt(uint64_t aValue) {
  NS_ASSERTION(aValue <= PR_UINT32_MAX, "Msg key value too big!");
  return aValue;
}

NS_MSG_BASE uint32_t msgKeyToInt(nsMsgKey aMsgKey) { return (uint32_t)aMsgKey; }

// Helper function to extract a query qualifier.
nsCString MsgExtractQueryPart(const nsACString& spec,
                              const char* queryToExtract) {
  nsCString queryPart;
  int32_t queryIndex = PromiseFlatCString(spec).Find(queryToExtract);
  if (queryIndex == kNotFound) return queryPart;

  int32_t queryEnd = spec.FindChar('&', queryIndex + 1);
  if (queryEnd == kNotFound) queryEnd = spec.FindChar('?', queryIndex + 1);
  if (queryEnd == kNotFound) {
    // Nothing follows, so return from where the query qualifier started.
    queryPart.Assign(Substring(spec, queryIndex));
  } else {
    // Return the substring that represents the query qualifier.
    queryPart.Assign(Substring(spec, queryIndex, queryEnd - queryIndex));
  }
  return queryPart;
}

// Helper function to remove query part from URL spec or path.
void MsgRemoveQueryPart(nsCString& aSpec) {
  // Sadly the query part can have different forms, these were seen
  // "in the wild", even with two ?:
  // /;section=2?part=1.2&filename=A01.JPG
  // ?section=2?part=1.2&filename=A01.JPG&type=image/jpeg&filename=A01.JPG
  // ?header=quotebody/;section=2.2?part=1.2.2&filename=lijbmghmkilicioj.png
  // ?part=1.2&type=image/jpeg&filename=IMG_C0030.jpg
  // ?header=quotebody&part=1.2&filename=lijbmghmkilicioj.png

  // Truncate path at the first of /; or ?
  int32_t ind = aSpec.FindChar('?');
  if (ind != kNotFound) aSpec.SetLength(ind);
  ind = aSpec.Find("/;");
  if (ind != kNotFound) aSpec.SetLength(ind);
}

// Perform C-style string escaping.
// e.g. "foo\r\n" => "foo\\r\\n"
// (See also CEscape(), in protobuf, for similar function).
nsCString CEscapeString(nsACString const& s) {
  nsCString out;
  for (size_t i = 0; i < s.Length(); ++i) {
    char c = s[i];
    if (c & 0x80) {
      out.AppendPrintf("\\x%02x", (uint8_t)c);
      continue;
    }
    switch (c) {
      case '\a':
        out += "\\a";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      case '\v':
        out += "\\v";
        break;
      default:
        if (c < ' ') {
          out.AppendPrintf("\\x%02x", (uint8_t)c);
        } else {
          out += c;
        }
        break;
    }
  }
  return out;
}

nsresult SyncCopyStream(nsIInputStream* src, nsIOutputStream* dest,
                        uint64_t& bytesCopied, size_t bufSize) {
  mozilla::Buffer<char> buf(bufSize);
  nsresult rv;

  bytesCopied = 0;
  while (1) {
    uint32_t numRead;
    rv = src->Read(buf.Elements(), buf.Length(), &numRead);
    NS_ENSURE_SUCCESS(rv, rv);
    if (numRead == 0) {
      break;  // EOF.
    }
    uint32_t pos = 0;
    while (pos < numRead) {
      uint32_t n;
      rv = dest->Write(&buf[pos], numRead - pos, &n);
      NS_ENSURE_SUCCESS(rv, rv);
      pos += n;
      bytesCopied += n;
    }
  }
  return NS_OK;
}
