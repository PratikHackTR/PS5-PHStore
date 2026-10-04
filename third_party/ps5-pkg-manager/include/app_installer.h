#ifndef APP_INSTALLER_H
#define APP_INSTALLER_H

#define PKGMGR_TITLE_ID "PKGM00001"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Installs the PKG Manager shortcut to the PS5 home screen (Media tab)
 * if it is not already installed or if files (param.json, icon0.png) need updating.
 *
 * Returns 0 on success, negative error code on failure.
 */
int app_installer_install_if_needed(void);

/**
 * Force reinstall the PKG Manager app shortcut to the PS5 home screen.
 */
int app_installer_force_install(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_INSTALLER_H */
