/*
 * Copyright (c) 1996, 2017, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#include "awt.h"
#include "awt_FileDialog.h"
#include "awt_Dialog.h"
#include "awt_Toolkit.h"
#include "ComCtl32Util.h"
#include <commdlg.h>
#include <cderr.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>

/************************************************************************
 * AwtFileDialog fields
 */

/* WFileDialogPeer ids */
jfieldID AwtFileDialog::parentID;
jfieldID AwtFileDialog::fileFilterID;
jmethodID AwtFileDialog::setHWndMID;
jmethodID AwtFileDialog::handleSelectedMID;
jmethodID AwtFileDialog::handleCancelMID;
jmethodID AwtFileDialog::checkFilenameFilterMID;
jmethodID AwtFileDialog::isMultipleModeMID;

/* FileDialog ids */
jfieldID AwtFileDialog::modeID;
jfieldID AwtFileDialog::dirID;
jfieldID AwtFileDialog::fileID;
jfieldID AwtFileDialog::filterID;

/* Localized filter string */
#define MAX_FILTER_STRING       128
static TCHAR s_fileFilterString[MAX_FILTER_STRING];
/* Non-localized suffix of the filter string */
static const TCHAR s_additionalString[] = TEXT(" (*.*)\0*.*\0");

// Default limit of the output buffer.
#define SINGLE_MODE_BUFFER_LIMIT     MAX_PATH+1
#define MULTIPLE_MODE_BUFFER_LIMIT   32768

// The name of the property holding the pointer to the OPENFILENAME structure.
static LPCTSTR OpenFileNameProp = TEXT("AWT_OFN");

/***********************************************************************/

void
AwtFileDialog::Initialize(JNIEnv *env, jstring filterDescription)
{
    int length = env->GetStringLength(filterDescription);
    DASSERT(length + 1 < MAX_FILTER_STRING);
    LPCTSTR tmp = JNU_GetStringPlatformChars(env, filterDescription, NULL);
    _tcscpy_s(s_fileFilterString, MAX_FILTER_STRING, tmp);
    JNU_ReleaseStringPlatformChars(env, filterDescription, tmp);

    //AdditionalString should be terminated by two NULL characters (Windows
    //requirement), so we have to organize the following cycle and use memcpy
    //unstead of, for example, strcat.
    LPTSTR s = s_fileFilterString;
    while (*s) {
        ++s;
        DASSERT(s < s_fileFilterString + MAX_FILTER_STRING);
    }
    DASSERT(s + sizeof(s_additionalString) < s_fileFilterString + MAX_FILTER_STRING);
    memcpy(s, s_additionalString, sizeof(s_additionalString));
}

LRESULT CALLBACK FileDialogWndProc(HWND hWnd, UINT message,
                                        WPARAM wParam, LPARAM lParam)
{
    JNIEnv *env = (JNIEnv *)JNU_GetEnv(jvm, JNI_VERSION_1_2);

    switch (message) {
        case WM_COMMAND: {
            if (LOWORD(wParam) == IDCANCEL)
            {
                // Unlike Print/Page dialogs, we only handle IDCANCEL here and
                // don't handle IDOK. This is because user can press OK button
                // when no file is selected, and the dialog is not closed. So
                // OK button is handled in the CDN_FILEOK notification handler
                // (see FileDialogHookProc below)
                jobject peer = (jobject)(::GetProp(hWnd, ModalDialogPeerProp));
                env->CallVoidMethod(peer, AwtFileDialog::setHWndMID, (jlong)0);
            }
            break;
        }
        case WM_SETICON: {
            return 0;
        }
    }

    WNDPROC lpfnWndProc = (WNDPROC)(::GetProp(hWnd, NativeDialogWndProcProp));
    return ComCtl32Util::GetInstance().DefWindowProc(lpfnWndProc, hWnd, message, wParam, lParam);
}

static UINT_PTR CALLBACK
FileDialogHookProc(HWND hdlg, UINT uiMsg, WPARAM wParam, LPARAM lParam)
{
    JNIEnv *env = (JNIEnv *)JNU_GetEnv(jvm, JNI_VERSION_1_2);

    TRY;

    HWND parent = ::GetParent(hdlg);

    switch(uiMsg) {
        case WM_INITDIALOG: {
            OPENFILENAME *ofn = (OPENFILENAME *)lParam;
            jobject peer = (jobject)(ofn->lCustData);
            env->CallVoidMethod(peer, AwtFileDialog::setHWndMID,
                                (jlong)parent);
            ::SetProp(parent, ModalDialogPeerProp, reinterpret_cast<HANDLE>(peer));

            // fix for 4508670 - disable CS_SAVEBITS
            DWORD style = ::GetClassLong(hdlg,GCL_STYLE);
            ::SetClassLong(hdlg,GCL_STYLE,style & ~CS_SAVEBITS);

            // set appropriate icon for parentless dialogs
            jobject awtParent = env->GetObjectField(peer, AwtFileDialog::parentID);
            if (awtParent == NULL) {
                ::SendMessage(parent, WM_SETICON, (WPARAM)ICON_BIG,
                              (LPARAM)AwtToolkit::GetInstance().GetAwtIcon());
            } else {
                AwtWindow *awtWindow = (AwtWindow *)JNI_GET_PDATA(awtParent);
                ::SendMessage(parent, WM_SETICON, (WPARAM)ICON_BIG,
                                               (LPARAM)(awtWindow->GetHIcon()));
                ::SendMessage(parent, WM_SETICON, (WPARAM)ICON_SMALL,
                                             (LPARAM)(awtWindow->GetHIconSm()));
                env->DeleteLocalRef(awtParent);
            }

            // subclass dialog's parent to receive additional messages
            WNDPROC lpfnWndProc = ComCtl32Util::GetInstance().SubclassHWND(parent,
                                                                           FileDialogWndProc);
            ::SetProp(parent, NativeDialogWndProcProp, reinterpret_cast<HANDLE>(lpfnWndProc));

            ::SetProp(parent, OpenFileNameProp, (void *)lParam);

            break;
        }
        case WM_DESTROY: {
            HIMC hIMC = ::ImmGetContext(hdlg);
            if (hIMC != NULL) {
                ::ImmNotifyIME(hIMC, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
                ::ImmReleaseContext(hdlg, hIMC);
            }

            WNDPROC lpfnWndProc = (WNDPROC)(::GetProp(parent, NativeDialogWndProcProp));
            ComCtl32Util::GetInstance().UnsubclassHWND(parent,
                                                       FileDialogWndProc,
                                                       lpfnWndProc);
            ::RemoveProp(parent, ModalDialogPeerProp);
            ::RemoveProp(parent, NativeDialogWndProcProp);
            ::RemoveProp(parent, OpenFileNameProp);
            break;
        }
        case WM_NOTIFY: {
            OFNOTIFYEX *notifyEx = (OFNOTIFYEX *)lParam;
            if (notifyEx) {
                jobject peer = (jobject)(::GetProp(parent, ModalDialogPeerProp));
                if (notifyEx->hdr.code == CDN_INCLUDEITEM) {
                    LPITEMIDLIST pidl = (LPITEMIDLIST)notifyEx->pidl;
                    // Get the filename and directory
                    TCHAR szPath[MAX_PATH];
                    if (!::SHGetPathFromIDList(pidl, szPath)) {
                        return TRUE;
                    }
                    jstring strPath = JNU_NewStringPlatform(env, szPath);
                    if (strPath == NULL) {
                        throw std::bad_alloc();
                    }
                    // Call FilenameFilter.accept with path and filename
                    UINT uRes = (env->CallBooleanMethod(peer,
                        AwtFileDialog::checkFilenameFilterMID, strPath) == JNI_TRUE);
                    env->DeleteLocalRef(strPath);
                    return uRes;
                } else if (notifyEx->hdr.code == CDN_FILEOK) {
                    // This notification is sent when user selects some file and presses
                    // OK button; it is not sent when no file is selected. So it's time
                    // to unblock all the windows blocked by this dialog as it will
                    // be closed soon
                    env->CallVoidMethod(peer, AwtFileDialog::setHWndMID, (jlong)0);
                } else if (notifyEx->hdr.code == CDN_SELCHANGE) {
                    // reallocate the buffer if the buffer is too small
                    LPOPENFILENAME lpofn = (LPOPENFILENAME)GetProp(parent, OpenFileNameProp);

                    UINT nLength = CommDlg_OpenSave_GetSpec(parent, NULL, 0) +
                                   CommDlg_OpenSave_GetFolderPath(parent, NULL, 0);

                    if (lpofn->nMaxFile < nLength)
                    {
                        // allocate new buffer
                        LPTSTR newBuffer = new TCHAR[nLength];

                        if (newBuffer) {
                            memset(newBuffer, 0, nLength * sizeof(TCHAR));
                            LPTSTR oldBuffer = lpofn->lpstrFile;
                            lpofn->lpstrFile = newBuffer;
                            lpofn->nMaxFile = nLength;
                            // free the previously allocated buffer
                            if (oldBuffer) {
                                delete[] oldBuffer;
                            }

                        }
                    }
                }
            }
            break;
        }
    }

    return FALSE;

    CATCH_BAD_ALLOC_RET(TRUE);
}

struct FileDialogData {
    IFileDialog *fileDialog;
    LPTSTR *result;
    UINT resultSize;
    jobject peer;
};

void GetSelectedResults(FileDialogData *data) {
    IFileOpenDialog *fileOpenDialog = NULL;
    if (!SUCCEEDED(data->fileDialog->QueryInterface(IID_PPV_ARGS(&fileOpenDialog))))
        return;

    LPTSTR resultBuffer = NULL;
    UINT currentOffset = 0;
    IShellItemArray *psia;
    if (SUCCEEDED(fileOpenDialog->GetSelectedItems(&psia))) {
        DWORD itemsCount;
        if (SUCCEEDED(psia->GetCount(&itemsCount))) {
            UINT maxBufferSize = (MAX_PATH + 1) * itemsCount + 1;
            resultBuffer = new TCHAR[maxBufferSize];
            data->resultSize = maxBufferSize;
            for (DWORD i = 0; i < itemsCount; i++) {
                IShellItem *psi;
                if (SUCCEEDED(psia->GetItemAt(i, &psi))) {
                    if (i == 0 && itemsCount > 1) {
                        IShellItem *psiParent;
                        VERIFY(SUCCEEDED(psi->GetParent(&psiParent)));

                        LPTSTR filePath;
                        if (SUCCEEDED(psiParent->GetDisplayName(SIGDN_FILESYSPATH, &filePath))) {
                            size_t filePathLength = _tcslen(filePath);
                            _tcsncpy(resultBuffer + currentOffset, filePath, filePathLength);
                            resultBuffer[currentOffset + filePathLength] = _T('\0');
                            currentOffset += filePathLength + 1;
                            CoTaskMemFree(filePath);
                        }
                        psiParent->Release();
                    }

                    LPTSTR filePath;
                    SIGDN displayForm = itemsCount > 1 ? SIGDN_PARENTRELATIVE : SIGDN_FILESYSPATH;
                    if (SUCCEEDED(psi->GetDisplayName(displayForm, &filePath))) {
                        size_t filePathLength = _tcslen(filePath);
                        _tcsncpy(resultBuffer + currentOffset, filePath, filePathLength);
                        resultBuffer[currentOffset + filePathLength] = _T('\0');
                        currentOffset += filePathLength + 1;
                        CoTaskMemFree(filePath);
                    }
                    psi->Release();
                }
            }
            resultBuffer[currentOffset] = _T('\0');
            resultBuffer[currentOffset + 1] = _T('\0');
            *(data->result) = resultBuffer;
        }
        psia->Release();
        data->fileDialog->Close(S_OK);
    }
    fileOpenDialog->Release();
}

LRESULT CALLBACK
FileDialogSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    JNIEnv *env = (JNIEnv *)JNU_GetEnv(jvm, JNI_VERSION_1_2);

    TRY;

    HWND parent = ::GetParent(hWnd);

    switch (uMsg) {
        case WM_COMMAND: {
            if (HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == IDOK) {
                GetSelectedResults((FileDialogData*) dwRefData);
            }
            if (LOWORD(wParam) == IDCANCEL) {
                jobject peer = (jobject) (::GetProp(hWnd, ModalDialogPeerProp));
                env->CallVoidMethod(peer, AwtFileDialog::setHWndMID, (jlong) 0);
            }
            break;
        }
        case WM_SETICON: {
            return 0;
        }
        case WM_DESTROY: {
            FileDialogData *data = (FileDialogData*) dwRefData;
            if (*data->result == NULL) {
                GetSelectedResults(data);
            }

            HIMC hIMC = ::ImmGetContext(hWnd);
            if (hIMC != NULL) {
                ::ImmNotifyIME(hIMC, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
                ::ImmReleaseContext(hWnd, hIMC);
            }

            RemoveWindowSubclass(hWnd, &FileDialogSubclassProc, uIdSubclass);

            ::RemoveProp(parent, ModalDialogPeerProp);
            break;
        }
    }

    return DefSubclassProc(hWnd, uMsg, wParam, lParam);

    CATCH_BAD_ALLOC_RET(TRUE);
}

class CDialogEventHandler : public IFileDialogEvents
{
public:
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv)
    {
        static const QITAB qit[] = {
                QITABENT(CDialogEventHandler, IFileDialogEvents),
                { 0 },
        };
        return QISearch(this, qit, riid, ppv);
    }

    IFACEMETHODIMP_(ULONG) AddRef()
    {
        return InterlockedIncrement(&m_refCount);
    }

    IFACEMETHODIMP_(ULONG) Release()
    {
        long retVal = InterlockedDecrement(&m_refCount);
        if (!retVal)
            delete this;
        return retVal;
    }

    IFACEMETHODIMP OnFolderChange(IFileDialog *fileDialog) {
        if (!m_activated) {
            InitDialog(fileDialog);
            m_activated = true;
        }
        return S_OK;
    };

    IFACEMETHODIMP OnFileOk(IFileDialog *) { return S_OK; };
    IFACEMETHODIMP OnFolderChanging(IFileDialog *, IShellItem *) { return S_OK; };
    IFACEMETHODIMP OnHelp(IFileDialog *) { return S_OK; };
    IFACEMETHODIMP OnSelectionChange(IFileDialog *) { return S_OK; };
    IFACEMETHODIMP OnShareViolation(IFileDialog *, IShellItem *, FDE_SHAREVIOLATION_RESPONSE *) { return S_OK; };
    IFACEMETHODIMP OnTypeChange(IFileDialog *pfd) { return S_OK; };
    IFACEMETHODIMP OnOverwrite(IFileDialog *, IShellItem *, FDE_OVERWRITE_RESPONSE *) { return S_OK; };

    CDialogEventHandler(FileDialogData *data) : data(data), m_refCount(1), m_activated(false) { };
private:
    ~CDialogEventHandler() { };
    FileDialogData *data;
    bool m_activated;
    long m_refCount;

    void InitDialog(IFileDialog *fileDialog) {
        JNIEnv *env = (JNIEnv *)JNU_GetEnv(jvm, JNI_VERSION_1_2);

        IOleWindow *pWindow = NULL;
        HRESULT hr = fileDialog->QueryInterface(IID_PPV_ARGS(&pWindow));

        if (!SUCCEEDED(hr))
            return;

        TRY;

        HWND hdlg;
        VERIFY(SUCCEEDED(pWindow->GetWindow(&hdlg)));
        pWindow->Release();

        HWND parent = ::GetParent(hdlg);
        jobject peer = data->peer;
        env->CallVoidMethod(peer, AwtFileDialog::setHWndMID, (jlong)parent);
        ::SetProp(parent, ModalDialogPeerProp, reinterpret_cast<HANDLE>(peer));

        // fix for 4508670 - disable CS_SAVEBITS
        DWORD style = ::GetClassLong(hdlg, GCL_STYLE);
        ::SetClassLong(hdlg, GCL_STYLE, style & ~CS_SAVEBITS);

        // set appropriate icon for parentless dialogs
        jobject awtParent = env->GetObjectField(peer, AwtFileDialog::parentID);
        if (awtParent == NULL) {
            ::SendMessage(parent, WM_SETICON, (WPARAM)ICON_BIG,
                          (LPARAM)AwtToolkit::GetInstance().GetAwtIcon());
        } else {
            AwtWindow *awtWindow = (AwtWindow *)JNI_GET_PDATA(awtParent);
            ::SendMessage(parent, WM_SETICON, (WPARAM)ICON_BIG,
                          (LPARAM)(awtWindow->GetHIcon()));
            ::SendMessage(parent, WM_SETICON, (WPARAM)ICON_SMALL,
                          (LPARAM)(awtWindow->GetHIconSm()));
            env->DeleteLocalRef(awtParent);
        }

        SetWindowSubclass(hdlg, &FileDialogSubclassProc, 0, (DWORD_PTR) data);

        CATCH_BAD_ALLOC;
    }
};

HRESULT CDialogEventHandler_CreateInstance(FileDialogData *data, REFIID riid, void **ppv)
{
    *ppv = NULL;
    CDialogEventHandler *pDialogEventHandler = new CDialogEventHandler(data);
    HRESULT hr = pDialogEventHandler->QueryInterface(riid, ppv);
    pDialogEventHandler->Release();
    return hr;
}

COMDLG_FILTERSPEC *CreateFilterSpec(UINT *count) {
    UINT filterCount = 0;
    for (UINT index = 0; index < MAX_FILTER_STRING - 1; index++) {
        if (s_fileFilterString[index] == _T('\0')) {
            filterCount++;
            if (s_fileFilterString[index + 1] == _T('\0'))
                break;
        }
    }
    filterCount /= 2;
    COMDLG_FILTERSPEC *filterSpec = new COMDLG_FILTERSPEC[filterCount];
    UINT currentIndex = 0;
    TCHAR *currentStart = s_fileFilterString;
    for (UINT index = 0; index < MAX_FILTER_STRING - 1; index++) {
        if (s_fileFilterString[index] == _T('\0')) {
            if (currentIndex & 1) {
                filterSpec[currentIndex / 2].pszSpec = currentStart;
            } else {
                filterSpec[currentIndex / 2].pszName = currentStart;
            }
            currentStart = s_fileFilterString + index + 1;
            currentIndex++;
            if (s_fileFilterString[index + 1] == _T('\0'))
                break;
        }
    }
    *count = filterCount;
    return filterSpec;
}

IShellItem *CreateShellItem(LPTSTR path) {
    size_t pathLength = _tcslen(path);
    for (size_t index = 0; index < pathLength; index++) {
        if (path[index] == _T('/'))
            path[index] = _T('\\');
    }

    IShellItem *shellItem;
    HRESULT hr = ::SHCreateItemInKnownFolder(FOLDERID_ComputerFolder, 0, path, IID_PPV_ARGS(&shellItem));
    if (SUCCEEDED(hr) && shellItem != NULL)
        return shellItem;
    return NULL;
}

LPTSTR GetShortName(LPTSTR path) {
    IShellItem *shellItem = CreateShellItem(path);
    if (shellItem == NULL)
        return NULL;

    LPTSTR shortName = NULL;
    shellItem->GetDisplayName(SIGDN_PARENTRELATIVE, &shortName);
    shellItem->Release();
    return shortName;
}

void
AwtFileDialog::Show(void *p)
{
    JNIEnv *env = (JNIEnv *)JNU_GetEnv(jvm, JNI_VERSION_1_2);
    jobject peer;
    LPTSTR currentDirectory = NULL;
    jint mode = 0;
    BOOL result = FALSE;
    DWORD dlgerr;
    jstring directory = NULL;
    jstring title = NULL;
    jstring file = NULL;
    jobject fileFilter = NULL;
    jobject target = NULL;
    jobject parent = NULL;
    AwtComponent* awtParent = NULL;
    jboolean multipleMode = JNI_FALSE;

    IFileDialog *pfd = NULL;
    IFileDialogEvents *pfde = NULL;
    DWORD dwCookie;
    IShellItem *psiResult = NULL;
    LPTSTR pszFilePath = NULL;
    COMDLG_FILTERSPEC *fileTypes = NULL;
    FileDialogData data;

    OPENFILENAME ofn;
    memset(&ofn, 0, sizeof(ofn));

    peer = (jobject)p;

    static BOOL useNewAPI = JNU_CallStaticMethodByName(env, NULL,
            "sun/awt/windows/WFileDialogPeer", "isUseNewAPI", "()Z").z == JNI_TRUE;
    try {
        DASSERT(peer);

        target = env->GetObjectField(peer, AwtObject::targetID);
        parent = env->GetObjectField(peer, AwtFileDialog::parentID);
        if (parent != NULL) {
            awtParent = (AwtComponent *)JNI_GET_PDATA(parent);
        }
//      DASSERT(awtParent);
        title = (jstring)(env)->GetObjectField(target, AwtDialog::titleID);
        HWND hwndOwner = awtParent ? awtParent->GetHWnd() : NULL;

        if (title == NULL || env->GetStringLength(title)==0) {
            title = JNU_NewStringPlatform(env, L" ");
            if (title == NULL) {
                throw std::bad_alloc();
            }
        }

        JavaStringBuffer titleBuffer(env, title);
        directory =
            (jstring)env->GetObjectField(target, AwtFileDialog::dirID);
        JavaStringBuffer directoryBuffer(env, directory);

        multipleMode = env->CallBooleanMethod(peer, AwtFileDialog::isMultipleModeMID);

        UINT bufferLimit;
        if (multipleMode == JNI_TRUE) {
            bufferLimit = MULTIPLE_MODE_BUFFER_LIMIT;
        } else {
            bufferLimit = SINGLE_MODE_BUFFER_LIMIT;
        }
        LPTSTR fileBuffer = new TCHAR[bufferLimit];
        memset(fileBuffer, 0, bufferLimit * sizeof(TCHAR));

        file = (jstring)env->GetObjectField(target, AwtFileDialog::fileID);
        if (file != NULL) {
            LPCTSTR tmp = JNU_GetStringPlatformChars(env, file, NULL);
            _tcsncpy(fileBuffer, tmp, bufferLimit - 2); // the fileBuffer is double null terminated string
            JNU_ReleaseStringPlatformChars(env, file, tmp);
        } else {
            fileBuffer[0] = _T('\0');
        }

        fileFilter = env->GetObjectField(peer, AwtFileDialog::fileFilterID);

        if (!useNewAPI) {
            ofn.lStructSize = sizeof(ofn);
            ofn.lpstrFilter = s_fileFilterString;
            ofn.nFilterIndex = 1;
            /*
              Fix for 6488834.
              To disable Win32 native parent modality we have to set
              hwndOwner field to either NULL or some hidden window. For
              parentless dialogs we use NULL to show them in the taskbar,
              and for all other dialogs AwtToolkit's HWND is used.
            */
            if (awtParent != NULL) {
                ofn.hwndOwner = AwtToolkit::GetInstance().GetHWnd();
            } else {
                ofn.hwndOwner = NULL;
            }
            ofn.lpstrFile = fileBuffer;
            ofn.nMaxFile = bufferLimit;
            ofn.lpstrTitle = titleBuffer;
            ofn.lpstrInitialDir = directoryBuffer;
            ofn.Flags = OFN_LONGNAMES | OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY |
                        OFN_ENABLEHOOK | OFN_EXPLORER | OFN_ENABLESIZING;

            if (!JNU_IsNull(env,fileFilter)) {
                ofn.Flags |= OFN_ENABLEINCLUDENOTIFY;
            }
            ofn.lCustData = (LPARAM)peer;
            ofn.lpfnHook = (LPOFNHOOKPROC)FileDialogHookProc;

            if (multipleMode == JNI_TRUE) {
                ofn.Flags |= OFN_ALLOWMULTISELECT;
            }

            // Save current directory, so we can reset if it changes.
            currentDirectory = new TCHAR[MAX_PATH+1];

            VERIFY(::GetCurrentDirectory(MAX_PATH, currentDirectory) > 0);
        }

        mode = env->GetIntField(target, AwtFileDialog::modeID);

        AwtDialog::CheckInstallModalHook();

        if (useNewAPI) {
            GUID fileDialogMode = mode == java_awt_FileDialog_LOAD ? CLSID_FileOpenDialog : CLSID_FileSaveDialog;
            VERIFY(SUCCEEDED(::CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)));
            VERIFY(SUCCEEDED(::CoCreateInstance(fileDialogMode, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd))));

            data.fileDialog = pfd;
            data.result = &pszFilePath;
            data.peer = peer;
            VERIFY(SUCCEEDED(CDialogEventHandler_CreateInstance(&data, IID_PPV_ARGS(&pfde))));
            VERIFY(SUCCEEDED(pfd->Advise(pfde, &dwCookie)));

            DWORD dwFlags;
            VERIFY(SUCCEEDED(pfd->GetOptions(&dwFlags)));
            dwFlags |= FOS_FORCEFILESYSTEM;
            if (multipleMode == JNI_TRUE) {
                dwFlags |= FOS_ALLOWMULTISELECT;
            }
            VERIFY(SUCCEEDED(pfd->SetOptions(dwFlags)));

            VERIFY(SUCCEEDED(pfd->SetTitle(titleBuffer)));

            UINT filterCount;
            fileTypes = CreateFilterSpec(&filterCount);

            VERIFY(SUCCEEDED(pfd->SetFileTypes(filterCount, fileTypes)));
            VERIFY(SUCCEEDED(pfd->SetFileTypeIndex(1)));

            IShellItem *directoryItem = CreateShellItem(directoryBuffer);
            if (directoryItem != NULL) {
                pfd->SetFolder(directoryItem);
                directoryItem->Release();
            }
            LPTSTR shortName = GetShortName(fileBuffer);
            if (shortName != NULL) {
                VERIFY(SUCCEEDED(pfd->SetFileName(shortName)));
                CoTaskMemFree(shortName);
            }
        }

        if (useNewAPI) {
            if (mode == java_awt_FileDialog_LOAD) {
                result = SUCCEEDED(pfd->Show(NULL));
                if (result) {
                    result = pszFilePath != NULL;
                    if (!result) {
                        result = SUCCEEDED(pfd->GetResult(&psiResult));
                        if (result) {
                            LPTSTR filePath;
                            result = SUCCEEDED(psiResult->GetDisplayName(SIGDN_FILESYSPATH, &filePath));
                            size_t filePathLength = _tcslen(filePath);
                            pszFilePath = new TCHAR[filePathLength + 1];
                            _tcscpy_s(pszFilePath, filePathLength + 1, filePath);
                            ::CoTaskMemFree(filePath);
                        }
                    }
                }
            } else {
                result = SUCCEEDED(pfd->Show(NULL));
                if (result) {
                    result = SUCCEEDED(pfd->GetResult(&psiResult));
                    if (result) {
                        LPTSTR filePath;
                        result = SUCCEEDED(psiResult->GetDisplayName(SIGDN_FILESYSPATH, &filePath));
                        size_t filePathLength = _tcslen(filePath);
                        pszFilePath = new TCHAR[filePathLength + 1];
                        _tcscpy_s(pszFilePath, filePathLength + 1, filePath);
                        ::CoTaskMemFree(filePath);
                    }
                }
            }
        } else {
            // show the Win32 file dialog
            if (mode == java_awt_FileDialog_LOAD) {
                result = ::GetOpenFileName(&ofn);
            } else {
                result = ::GetSaveFileName(&ofn);
            }
            // Fix for 4181310: FileDialog does not show up.
            // If the dialog is not shown because of invalid file name
            // replace the file name by empty string.
            if (!result) {
                dlgerr = ::CommDlgExtendedError();
                if (dlgerr == FNERR_INVALIDFILENAME) {
                    _tcscpy_s(fileBuffer, bufferLimit, TEXT(""));
                    if (mode == java_awt_FileDialog_LOAD) {
                        result = ::GetOpenFileName(&ofn);
                    } else {
                        result = ::GetSaveFileName(&ofn);
                    }
                }
            }
        }

        AwtDialog::CheckUninstallModalHook();

        DASSERT(env->GetLongField(peer, AwtComponent::hwndID) == 0L);

        AwtDialog::ModalActivateNextWindow(NULL, target, peer);

        if (useNewAPI) {
            VERIFY(::SetCurrentDirectory(currentDirectory));
        }

        // Report result to peer.
        if (result) {
            jint length;
            if (useNewAPI) {
                length = (jint) GetBufferLength(pszFilePath, data.resultSize);
            } else {
                length = multipleMode
                         ? (jint) GetBufferLength(ofn.lpstrFile, ofn.nMaxFile)
                         : (jint) _tcslen(ofn.lpstrFile);
            }

            jcharArray jnames = env->NewCharArray(length);
            if (jnames == NULL) {
                throw std::bad_alloc();
            }

            if (useNewAPI) {
                env->SetCharArrayRegion(jnames, 0, length, (jchar *) pszFilePath);
            } else {
                env->SetCharArrayRegion(jnames, 0, length, (jchar *) ofn.lpstrFile);
            }
            env->CallVoidMethod(peer, AwtFileDialog::handleSelectedMID, jnames);
            env->DeleteLocalRef(jnames);
        } else {
            env->CallVoidMethod(peer, AwtFileDialog::handleCancelMID);
        }
        DASSERT(!safe_ExceptionOccurred(env));
    } catch (...) {

        if (useNewAPI) {
            if (psiResult)
                psiResult->Release();
            if (pfd) {
                pfd->Unadvise(dwCookie);
                pfd->Release();
            }
            if (pfde)
                pfde->Release();
            if (pszFilePath)
                delete [] pszFilePath;
            if (fileTypes)
                delete [] fileTypes;
            ::CoUninitialize();
        }

        env->DeleteLocalRef(target);
        env->DeleteLocalRef(parent);
        env->DeleteLocalRef(title);
        env->DeleteLocalRef(directory);
        env->DeleteLocalRef(file);
        env->DeleteLocalRef(fileFilter);
        env->DeleteGlobalRef(peer);

        delete[] currentDirectory;
        if (ofn.lpstrFile)
            delete[] ofn.lpstrFile;
        throw;
    }

    if (useNewAPI) {
        if (psiResult)
            psiResult->Release();
        if (pfd) {
            pfd->Unadvise(dwCookie);
            pfd->Release();
        }
        if (pfde)
            pfde->Release();
        if (pszFilePath)
            delete[] pszFilePath;
        if (fileTypes)
            delete[] fileTypes;
        CoUninitialize();
    }

    env->DeleteLocalRef(target);
    env->DeleteLocalRef(parent);
    env->DeleteLocalRef(title);
    env->DeleteLocalRef(directory);
    env->DeleteLocalRef(file);
    env->DeleteLocalRef(fileFilter);
    env->DeleteGlobalRef(peer);

    delete[] currentDirectory;
    if (ofn.lpstrFile)
        delete[] ofn.lpstrFile;
}

BOOL AwtFileDialog::InheritsNativeMouseWheelBehavior() {return true;}

void AwtFileDialog::_DisposeOrHide(void *param)
{
    JNIEnv *env = (JNIEnv *)JNU_GetEnv(jvm, JNI_VERSION_1_2);

    jobject self = (jobject)param;

    HWND hdlg = (HWND)(env->GetLongField(self, AwtComponent::hwndID));
    if (::IsWindow(hdlg))
    {
        ::SendMessage(hdlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, 0),
                      (LPARAM)hdlg);
    }

    env->DeleteGlobalRef(self);
}

void AwtFileDialog::_ToFront(void *param)
{
    JNIEnv *env = (JNIEnv *)JNU_GetEnv(jvm, JNI_VERSION_1_2);

    jobject self = (jobject)param;
    HWND hdlg = (HWND)(env->GetLongField(self, AwtComponent::hwndID));
    if (::IsWindow(hdlg))
    {
        ::SetWindowPos(hdlg, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }

    env->DeleteGlobalRef(self);
}

void AwtFileDialog::_ToBack(void *param)
{
    JNIEnv *env = (JNIEnv *)JNU_GetEnv(jvm, JNI_VERSION_1_2);

    jobject self = (jobject)param;
    HWND hdlg = (HWND)(env->GetLongField(self, AwtComponent::hwndID));
    if (::IsWindow(hdlg))
    {
        ::SetWindowPos(hdlg, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    env->DeleteGlobalRef(self);
}

// Returns the length of the double null terminated output buffer
UINT AwtFileDialog::GetBufferLength(LPTSTR buffer, UINT limit)
{
    UINT index = 0;
    while ((index < limit) &&
           (buffer[index] != NULL || buffer[index+1] != NULL))
    {
        index++;
    }
    return index;
}

/************************************************************************
 * WFileDialogPeer native methods
 */

extern "C" {

JNIEXPORT void JNICALL
Java_sun_awt_windows_WFileDialogPeer_initIDs(JNIEnv *env, jclass cls)
{
    TRY;

    AwtFileDialog::parentID =
        env->GetFieldID(cls, "parent", "Lsun/awt/windows/WComponentPeer;");
    DASSERT(AwtFileDialog::parentID != NULL);
    CHECK_NULL(AwtFileDialog::parentID);

    AwtFileDialog::fileFilterID =
        env->GetFieldID(cls, "fileFilter", "Ljava/io/FilenameFilter;");
    DASSERT(AwtFileDialog::fileFilterID != NULL);
    CHECK_NULL(AwtFileDialog::fileFilterID);

    AwtFileDialog::setHWndMID = env->GetMethodID(cls, "setHWnd", "(J)V");
    DASSERT(AwtFileDialog::setHWndMID != NULL);
    CHECK_NULL(AwtFileDialog::setHWndMID);

    AwtFileDialog::handleSelectedMID =
        env->GetMethodID(cls, "handleSelected", "([C)V");
    DASSERT(AwtFileDialog::handleSelectedMID != NULL);
    CHECK_NULL(AwtFileDialog::handleSelectedMID);

    AwtFileDialog::handleCancelMID =
        env->GetMethodID(cls, "handleCancel", "()V");
    DASSERT(AwtFileDialog::handleCancelMID != NULL);
    CHECK_NULL(AwtFileDialog::handleCancelMID);

    AwtFileDialog::checkFilenameFilterMID =
        env->GetMethodID(cls, "checkFilenameFilter", "(Ljava/lang/String;)Z");
    DASSERT(AwtFileDialog::checkFilenameFilterMID != NULL);
    CHECK_NULL(AwtFileDialog::checkFilenameFilterMID);

    AwtFileDialog::isMultipleModeMID = env->GetMethodID(cls, "isMultipleMode", "()Z");
    DASSERT(AwtFileDialog::isMultipleModeMID != NULL);
    CHECK_NULL(AwtFileDialog::isMultipleModeMID);

    /* java.awt.FileDialog fields */
    cls = env->FindClass("java/awt/FileDialog");
    CHECK_NULL(cls);

    AwtFileDialog::modeID = env->GetFieldID(cls, "mode", "I");
    DASSERT(AwtFileDialog::modeID != NULL);
    CHECK_NULL(AwtFileDialog::modeID);

    AwtFileDialog::dirID = env->GetFieldID(cls, "dir", "Ljava/lang/String;");
    DASSERT(AwtFileDialog::dirID != NULL);
    CHECK_NULL(AwtFileDialog::dirID);

    AwtFileDialog::fileID = env->GetFieldID(cls, "file", "Ljava/lang/String;");
    DASSERT(AwtFileDialog::fileID != NULL);
    CHECK_NULL(AwtFileDialog::fileID);

    AwtFileDialog::filterID =
        env->GetFieldID(cls, "filter", "Ljava/io/FilenameFilter;");
    DASSERT(AwtFileDialog::filterID != NULL);

    CATCH_BAD_ALLOC;
}

JNIEXPORT void JNICALL
Java_sun_awt_windows_WFileDialogPeer_setFilterString(JNIEnv *env, jclass cls,
                                                     jstring filterDescription)
{
    TRY;

    AwtFileDialog::Initialize(env, filterDescription);

    CATCH_BAD_ALLOC;
}

JNIEXPORT void JNICALL
Java_sun_awt_windows_WFileDialogPeer__1show(JNIEnv *env, jobject peer)
{
    TRY;

    /*
     * Fix for 4906972.
     * 'peer' reference has to be global as it's used further in another thread.
     */
    jobject peerGlobal = env->NewGlobalRef(peer);

    if (!AwtToolkit::GetInstance().PostMessage(WM_AWT_INVOKE_METHOD,
                             (WPARAM)AwtFileDialog::Show, (LPARAM)peerGlobal)) {
        env->DeleteGlobalRef(peerGlobal);
    }

    CATCH_BAD_ALLOC;
}

JNIEXPORT void JNICALL
Java_sun_awt_windows_WFileDialogPeer__1dispose(JNIEnv *env, jobject peer)
{
    TRY_NO_VERIFY;

    jobject peerGlobal = env->NewGlobalRef(peer);

    AwtToolkit::GetInstance().SyncCall(AwtFileDialog::_DisposeOrHide,
        (void *)peerGlobal);
    // peerGlobal ref is deleted in _DisposeOrHide

    CATCH_BAD_ALLOC;
}

JNIEXPORT void JNICALL
Java_sun_awt_windows_WFileDialogPeer__1hide(JNIEnv *env, jobject peer)
{
    TRY;

    jobject peerGlobal = env->NewGlobalRef(peer);

    AwtToolkit::GetInstance().SyncCall(AwtFileDialog::_DisposeOrHide,
        (void *)peerGlobal);
    // peerGlobal ref is deleted in _DisposeOrHide

    CATCH_BAD_ALLOC;
}

JNIEXPORT void JNICALL
Java_sun_awt_windows_WFileDialogPeer_toFront(JNIEnv *env, jobject peer)
{
    TRY;

    AwtToolkit::GetInstance().SyncCall(AwtFileDialog::_ToFront,
                                       (void *)(env->NewGlobalRef(peer)));
    // global ref is deleted in _ToFront

    CATCH_BAD_ALLOC;
}

JNIEXPORT void JNICALL
Java_sun_awt_windows_WFileDialogPeer_toBack(JNIEnv *env, jobject peer)
{
    TRY;

    AwtToolkit::GetInstance().SyncCall(AwtFileDialog::_ToBack,
                                       (void *)(env->NewGlobalRef(peer)));
    // global ref is deleted in _ToBack

    CATCH_BAD_ALLOC;
}

int ScaleDownX(int x, HWND hwnd) {
    int screen = AwtWin32GraphicsDevice::DeviceIndexForWindow(hwnd);
    Devices::InstanceAccess devices;
    AwtWin32GraphicsDevice* device = devices->GetDevice(screen);
    return device == NULL ? x : device->ScaleDownX(x);
}

int ScaleDownY(int y, HWND hwnd) {
    int screen = AwtWin32GraphicsDevice::DeviceIndexForWindow(hwnd);
    Devices::InstanceAccess devices;
    AwtWin32GraphicsDevice* device = devices->GetDevice(screen);
    return device == NULL ? y : device->ScaleDownY(y);
}

jobject AwtFileDialog::_GetLocationOnScreen(void *param)
{
    JNIEnv *env = (JNIEnv *)JNU_GetEnv(jvm, JNI_VERSION_1_2);

    jobject result = NULL;
    HWND hwnd = (HWND)env->GetLongField((jobject)param, AwtComponent::hwndID);

    if (::IsWindow(hwnd))
    {
        RECT rect;
        VERIFY(::GetWindowRect(hwnd, &rect));
        result = JNU_NewObjectByName(env, "java/awt/Point", "(II)V",
                       ScaleDownX(rect.left, hwnd), ScaleDownY(rect.top, hwnd));
    }

    if (result != NULL)
    {
        jobject resultRef = env->NewGlobalRef(result);
        env->DeleteLocalRef(result);
        return resultRef;
    }
    else
    {
        return NULL;
    }
}

/*
 * Class:     sun_awt_windows_WFileDialogPeer
 * Method:    getLocationOnScreen
 * Signature: ()Ljava/awt/Point;
 */
JNIEXPORT jobject JNICALL
Java_sun_awt_windows_WFileDialogPeer_getLocationOnScreen(JNIEnv *env,
                                                                 jobject peer) {
    TRY;

    jobject peerRef = env->NewGlobalRef(peer);
    jobject resultRef = (jobject)AwtToolkit::GetInstance().SyncCall(
        (void*(*)(void*))AwtFileDialog::_GetLocationOnScreen, (void *)peerRef);
    env->DeleteGlobalRef(peerRef);

    if (resultRef != NULL)
    {
        jobject result = env->NewLocalRef(resultRef);
        env->DeleteGlobalRef(resultRef);
        return result;
    }

    return NULL;

    CATCH_BAD_ALLOC_RET(NULL);
}

} /* extern "C" */
