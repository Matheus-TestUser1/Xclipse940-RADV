#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <xf86drm.h>
#include <amdgpu.h>

static void print_rc(const char *what, int rc)
{
    if (rc == 0)
        printf("%-44s : 0 (OK)\n", what);
    else
        printf("%-44s : %d (%s)\n", what, rc, strerror(-rc));
}

int main(void)
{
    const char *node = "/dev/dri/renderD128";
    int fd = -1;
    amdgpu_device_handle dev = NULL;
    uint32_t maj = 0, min = 0;

    uint32_t timeline_src = 0;
    uint32_t binary_dst = 0;
    uint32_t binary_src = 0;
    uint32_t timeline_dst = 0;

    int sf = -1, sf2 = -1;
    int rc = 1;

    printf("START sgpu_timeline_syncobj_probe\n");

    fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        printf("open(%s) failed: %s\n", node, strerror(errno));
        return 10;
    }
    printf("opened %s fd=%d\n", node, fd);

    rc = amdgpu_device_initialize(fd, &maj, &min, &dev);
    print_rc("amdgpu_device_initialize", rc);
    if (rc)
        goto out;
    printf("DRM version                                  : %u.%u\n", maj, min);

    rc = drmSyncobjCreate(fd, 0, &timeline_src);
    print_rc("drmSyncobjCreate(timeline_src)", rc);
    if (rc)
        goto out;

    rc = drmSyncobjCreate(fd, 0, &binary_dst);
    print_rc("drmSyncobjCreate(binary_dst)", rc);
    if (rc)
        goto out;

    uint64_t point7 = 7;
    rc = drmSyncobjTimelineSignal(fd, &timeline_src, &point7, 1);
    print_rc("drmSyncobjTimelineSignal(src, point=7)", rc);
    if (rc)
        goto out;

    uint64_t queried = 0;
    rc = drmSyncobjQuery(fd, &timeline_src, &queried, 1);
    print_rc("drmSyncobjQuery(timeline_src)", rc);
    if (rc)
        goto out;
    printf("timeline_src queried point                   : %llu\n",
           (unsigned long long)queried);

    rc = amdgpu_cs_syncobj_export_sync_file2(dev, timeline_src, 7, 0, &sf);
    print_rc("amdgpu_cs_syncobj_export_sync_file2(point=7)", rc);
    if (rc)
        goto out;
    printf("timeline point sync_file fd                  : %d\n", sf);

    rc = amdgpu_cs_syncobj_import_sync_file(dev, binary_dst, sf);
    print_rc("amdgpu_cs_syncobj_import_sync_file(binary)", rc);
    if (rc)
        goto out;

    rc = drmSyncobjExportSyncFile(fd, binary_dst, &sf2);
    print_rc("drmSyncobjExportSyncFile(binary_dst)", rc);
    if (rc)
        goto out;

    struct pollfd pfd = {
        .fd = sf2,
        .events = POLLIN,
    };
    int pr = poll(&pfd, 1, 1000);
    if (pr < 0) {
        printf("poll failed: %s\n", strerror(errno));
        rc = -errno;
        goto out;
    }
    printf("poll(exported timeline point, 1000ms)       : %d revents=0x%x\n",
           pr, pfd.revents);
    if (!(pr == 1 && (pfd.revents & POLLIN))) {
        puts("FAIL: timeline point -> sync_file did not become signaled.");
        rc = -ETIMEDOUT;
        goto out;
    }

    close(sf2);
    sf2 = -1;
    close(sf);
    sf = -1;

    /*
     * Now test the opposite primitive used by RADV zero-submit timeline signals:
     *
     *   destination timeline point <- source binary syncobj
     */
    rc = drmSyncobjCreate(fd, 0, &binary_src);
    print_rc("drmSyncobjCreate(binary_src)", rc);
    if (rc)
        goto out;

    rc = drmSyncobjCreate(fd, 0, &timeline_dst);
    print_rc("drmSyncobjCreate(timeline_dst)", rc);
    if (rc)
        goto out;

    rc = drmSyncobjSignal(fd, &binary_src, 1);
    print_rc("drmSyncobjSignal(binary_src)", rc);
    if (rc)
        goto out;

    rc = amdgpu_cs_syncobj_transfer(dev,
                                    timeline_dst, 11,
                                    binary_src, 0,
                                    0);
    print_rc("amdgpu_cs_syncobj_transfer(binary -> point=11)", rc);
    if (rc)
        goto out;

    uint32_t first = 0;
    uint64_t wait_point = 11;
    rc = drmSyncobjTimelineWait(fd, &timeline_dst, &wait_point, 1,
                                1000000000LL, 0, &first);
    print_rc("drmSyncobjTimelineWait(dst, point=11)", rc);
    if (rc)
        goto out;

    uint64_t dst_queried = 0;
    rc = drmSyncobjQuery(fd, &timeline_dst, &dst_queried, 1);
    print_rc("drmSyncobjQuery(timeline_dst)", rc);
    if (rc)
        goto out;
    printf("timeline_dst queried point                   : %llu\n",
           (unsigned long long)dst_queried);

    puts("PASS: timeline export_sync_file2 and syncobj transfer both work.");
    rc = 0;

out:
    if (sf2 >= 0)
        close(sf2);
    if (sf >= 0)
        close(sf);

    if (timeline_dst)
        drmSyncobjDestroy(fd, timeline_dst);
    if (binary_src)
        drmSyncobjDestroy(fd, binary_src);
    if (binary_dst)
        drmSyncobjDestroy(fd, binary_dst);
    if (timeline_src)
        drmSyncobjDestroy(fd, timeline_src);

    if (dev)
        amdgpu_device_deinitialize(dev);
    if (fd >= 0)
        close(fd);

    if (rc == 0)
        return 0;

    printf("FINAL rc=%d\n", rc);
    return 20;
}
