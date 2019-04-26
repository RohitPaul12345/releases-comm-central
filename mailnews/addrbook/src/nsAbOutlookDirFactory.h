/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef nsAbOutlookDirFactory_h___
#define nsAbOutlookDirFactory_h___

#include "nsIAbDirFactory.h"

class nsAbOutlookDirFactory : public nsIAbDirFactory {
 public:
  nsAbOutlookDirFactory(void);

  NS_DECL_ISUPPORTS
  NS_DECL_NSIABDIRFACTORY

 private:
  virtual ~nsAbOutlookDirFactory(void);
};

#endif  // nsAbOutlookDirFactory_h___
