#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <xf86drm.h>

static void print_rc(const char *what, int rc)
{
    if (rc == 0) {
        printf("%-34s : 0 (OK)\n", what);
    } else {
        printf("%-34s : %d (%s)\n", what, rc, strerror(-rc));
    }
}

int main(void)
{
    const char *node = "/dev/dri/renderD128";
    int fd = -1;
    uint32_t src = 0, dst = 0;
    int src_sync_file = -1, dst_sync_file = -1;
    int rc = 1;

    printf("START syncobj_sync_file_probe\n");

    fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        printf("open(%s) failed: %s\n", node, strerror(errno));
        return 10;
    }
    printf("opened %s fd=%d\n", node, fd);

    rc = drmSyncobjCreate(fd, 0, &src);
    print_rc("drmSyncobjCreate(src)", rc);
    if (rc)
        goto out;

    rc = drmSyncobjCreate(fd, 0, &dst);
    print_rc("drmSyncobjCreate(dst)", rc);
    if (rc)
        goto out;

    /* Give src a real signaled fence state. This probe is intentionally
     * capability-focused: does the SGPU DRM node support syncobj <-> sync_file
     * export/import at all? */
    rc = drmSyncobjSignal(fd, &src, 1);
    print_rc("drmSyncobjSignal(src)", rc);
    if (rc)
        goto out;

    rc = drmSyncobjExportSyncFile(fd, src, &src_sync_file);
    print_rc("drmSyncobjExportSyncFile(src)", rc);
    if (rc)
        goto out;
    printf("src sync_file fd                  : %d\n", src_sync_file);

    rc = drmSyncobjImportSyncFile(fd, dst, src_sync_file);
    print_rc("drmSyncobjImportSyncFile(dst)", rc);
    if (rc)
        goto out;

    rc = drmSyncobjExportSyncFile(fd, dst, &dst_sync_file);
    print_rc("drmSyncobjExportSyncFile(dst)", rc);
    if (rc)
        goto out;
    printf("dst sync_file fd                  : %d\n", dst_sync_file);

    struct pollfd pfd = {
        .fd = dst_sync_file,
        .events = POLLIN,
    };

    int pr = poll(&pfd, 1, 1000);
    if (pr < 0) {
        printf("poll(dst sync_file) failed: %s\n", strerror(errno));
        rc = -errno;
        goto out;
    }

    printf("poll(dst sync_file, 1000ms)       : %d revents=0x%x\n", pr, pfd.revents);

    if (pr == 1 && (pfd.revents & POLLIN)) {
        puts("PASS: syncobj <-> sync_file export/import works on renderD128.");
        rc = 0;
    } else {
        puts("FAIL: export/import calls returned success, but imported fence was not signaled.");
        rc = -ETIMEDOUT;
    }

out:
    if (dst_sync_file >= 0)
        close(dst_sync_file);
    if (src_sync_file >= 0)
        close(src_sync_file);
    if (dst)
        drmSyncobjDestroy(fd, dst);
    if (src)
        drmSyncobjDestroy(fd, src);
    if (fd >= 0)
        close(fd);

    if (rc == 0)
        return 0;

    printf("FINAL rc=%d\n", rc);
    return 20;
}
