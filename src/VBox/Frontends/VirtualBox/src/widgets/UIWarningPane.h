/** @file
 *
 * VBox frontends: Qt4 GUI ("VirtualBox"):
 * UIWarningPane class declaration
 */

/*
 * Copyright (C) 2009-2013 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#ifndef __UIWarningPane_h__
#define __UIWarningPane_h__

/* Global includes */
#include <QWidget>

/* Forward declarations: */
class QLabel;

/* Warning-pane prototype: */
class UIWarningPane: public QWidget
{
    Q_OBJECT;

public:

    /* Constructor: */
    UIWarningPane(QWidget *pParent = 0);

    /* API: Warning stuff: */
    void setWarningPixmap(const QPixmap &pixmap);
    void setWarningText(const QString &strText);

private:

    /* Helpers: Prepare stuff: */
    void prepare();
    void prepareContent();

    /* Variables: Widgets: */
    QLabel *m_pLabelIcon;
    QLabel *m_pLabelText;
};

#endif /* __UIWarningPane_h__ */
