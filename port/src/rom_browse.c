/**
 * rom_browse.c - the first-run "where is your cart?" file dialog.
 *
 * romdata.c calls portRomBrowse when no ROM turned up in any of the usual
 * places (argv, --rom-file, GE_ROM, midas.ini, ./Data). It returns 1 with the
 * picked path, or 0 on cancel / no dialog available. Kept apart from
 * romdata.c because <windows.h> and the game's ultra64 types don't mix.
 *
 *   Windows: the common Open dialog (comdlg32).
 *   Linux:   zenity, then kdialog, when either is installed.
 *
 * GE_NOROMDIALOG=1 skips it (headless runs, CI).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#endif

int portRomBrowse(char *out, int n)
{
    const char *e = getenv("GE_NOROMDIALOG");

    if (out == NULL || n <= 1 || (e != NULL && e[0] != '\0' && e[0] != '0'))
    {
        return 0;
    }
    out[0] = '\0';

#ifdef _WIN32
    {
        OPENFILENAMEA ofn;
        static int told;

        if (!told)
        {
            told = 1;
            MessageBoxA(NULL,
                        "Welcome! The game's assets are read from your own GoldenEye 007 (USA) cart image.\n\n"
                        "Next, pick your ROM file (.z64, .v64 or .n64). It is copied into the Data folder "
                        "next to the game, so you only do this once.",
                        "GoldenEye 007 - Select your ROM", MB_OK | MB_ICONINFORMATION);
        }
        memset(&ofn, 0, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.lpstrFilter = "N64 ROM images (*.z64;*.v64;*.n64)\0*.z64;*.v64;*.n64\0All files (*.*)\0*.*\0";
        ofn.lpstrFile = out;
        ofn.nMaxFile = (DWORD) n;
        ofn.lpstrTitle = "Select your GoldenEye 007 (USA) ROM for asset extraction";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
        if (GetOpenFileNameA(&ofn) && out[0] != '\0')
        {
            return 1;
        }
        return 0;
    }
#else
    {
        static const char *const cmds[] = {
            "zenity --file-selection --title='Select your GoldenEye 007 (USA) ROM for asset extraction' "
            "--file-filter='N64 ROM images | *.z64 *.v64 *.n64' --file-filter='All files | *' 2>/dev/null",
            "kdialog --title 'Select your GoldenEye 007 (USA) ROM for asset extraction' "
            "--getopenfilename . '*.z64 *.v64 *.n64' 2>/dev/null",
        };
        size_t i;

        for (i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        {
            FILE *p = popen(cmds[i], "r");
            int rc;
            size_t len;

            if (p == NULL)
            {
                continue;
            }
            if (fgets(out, n, p) == NULL)
            {
                out[0] = '\0';
            }
            rc = pclose(p);
            len = strlen(out);
            while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r'))
            {
                out[--len] = '\0';
            }
            if (len > 0)
            {
                return 1;
            }
            /* exit status 127: that tool isn't installed, try the next;
             * anything else is a cancel */
            if (rc != -1 && (rc >> 8) != 127)
            {
                return 0;
            }
        }
        return 0;
    }
#endif
}
