// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/iommu.h>
#include <linux/of_platform.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/arm-smccc.h>
#include <linux/dma-mapping.h>
#include <linux/dma-direct.h>
#include <linux/string.h>

/* ==========================================================
 * 快速測試開關
 * ========================================================== */
#define ENABLE_SMC_TEST 0  // 1: 啟動 SMC 驗證, 0: 關閉
#define ENABLE_LOG_TEST 0  // 1: 顯示所有 Log, 0: 關閉 (測效能必關)

static struct iommu_device *global_mmu;
static const struct dma_map_ops mock_dma_ops;

/* --- A) per-device hook --- */
struct mock_dev_hook {
    struct device *dev;
    const struct dma_map_ops *orig;
    struct list_head node;
};

static LIST_HEAD(hook_list);
static DEFINE_SPINLOCK(hook_lock);

static struct mock_dev_hook *hook_find_locked(struct device *dev)
{
    struct mock_dev_hook *h;
    list_for_each_entry(h, &hook_list, node) {
        if (h->dev == dev)
            return h;
    }
    return NULL;
}

static const struct dma_map_ops *hook_get_orig(struct device *dev)
{
    struct mock_dev_hook *h;
    unsigned long flags;
    const struct dma_map_ops *orig = NULL;

    spin_lock_irqsave(&hook_lock, flags);
    h = hook_find_locked(dev);
    if (h)
        orig = h->orig;
    spin_unlock_irqrestore(&hook_lock, flags);

    return orig;
}

static int hook_install(struct device *dev, const struct dma_map_ops *orig)
{
    struct mock_dev_hook *h;
    unsigned long flags;

    h = kzalloc(sizeof(*h), GFP_KERNEL);
    if (!h)
        return -ENOMEM;

    h->dev = dev;
    h->orig = orig;

    spin_lock_irqsave(&hook_lock, flags);
    if (hook_find_locked(dev)) {
        spin_unlock_irqrestore(&hook_lock, flags);
        kfree(h);
        return 0;
    }
    list_add_tail(&h->node, &hook_list);
    spin_unlock_irqrestore(&hook_lock, flags);

    return 0;
}

/* --- B) DMA-OPS Wrapper --- */

static dma_addr_t mock_map_page(struct device *dev, struct page *page,
                                unsigned long offset, size_t size,
                                enum dma_data_direction dir, unsigned long attrs)
{
    const struct dma_map_ops *orig = hook_get_orig(dev);
    dma_addr_t dma;
    phys_addr_t pa = page_to_phys(page) + offset;
    long smc_ret = 0;

#if ENABLE_SMC_TEST
    {
        struct arm_smccc_res res;
        arm_smccc_smc(0x82000000, (unsigned long)pa, pa, size, 0, 0, 0, 0, &res);
        smc_ret = (long)res.a0;
#if ENABLE_LOG_TEST
        if (smc_ret == -1)
            pr_emerg("Mock-IOMMU: Page SMC Call Failed (Unknown ID) PA: %pa\n", &pa);
        else
            pr_emerg("Mock-IOMMU: Page SMC Success, Ret: %ld, PA: %pa\n", smc_ret, &pa);
#endif
    }
#endif

    if (orig && orig->map_page) {
        dma = orig->map_page(dev, page, offset, size, dir, attrs);
    } else {
        if (!(attrs & DMA_ATTR_SKIP_CPU_SYNC))
            arch_sync_dma_for_device(pa, size, dir);
        dma = (dma_addr_t)pa;
    }

#if ENABLE_LOG_TEST
    pr_emerg("Mock-IOMMU(DMA-OPS): map_page dev=%s dma=0x%llx pa=%pa size=%zu\n",
             dev_name(dev), (u64)dma, &pa, size);
#endif

    return dma;
}

static void mock_unmap_page(struct device *dev, dma_addr_t addr, size_t size,
                            enum dma_data_direction dir, unsigned long attrs)
{
    const struct dma_map_ops *orig = hook_get_orig(dev);

    if (orig && orig->unmap_page) {
        orig->unmap_page(dev, addr, size, dir, attrs);
    } else {
        if (!(attrs & DMA_ATTR_SKIP_CPU_SYNC))
            arch_sync_dma_for_cpu((phys_addr_t)addr, size, dir);
    }

#if ENABLE_LOG_TEST
    pr_emerg("Mock-IOMMU(DMA-OPS): unmap_page dev=%s dma=0x%llx\n", dev_name(dev), (u64)addr);
#endif
}

static int mock_map_sg(struct device *dev, struct scatterlist *sgl, int nents,
                       enum dma_data_direction dir, unsigned long attrs)
{
    const struct dma_map_ops *orig = hook_get_orig(dev);
    int ret, i;
    struct scatterlist *sg;

    if (orig && orig->map_sg) {
        ret = orig->map_sg(dev, sgl, nents, dir, attrs);
    } else {
        for_each_sg(sgl, sg, nents, i) {
            sg_dma_address(sg) = sg_phys(sg);
            sg_dma_len(sg) = sg->length;
        }
        ret = nents;
    }

#if ENABLE_SMC_TEST
    {
        struct arm_smccc_res res;
        for_each_sg(sgl, sg, ret, i) {
            phys_addr_t pa = sg_phys(sg);
            arm_smccc_smc(0x82000000, (unsigned long)sg_dma_address(sg), 
                          pa, sg->length, 0, 0, 0, 0, &res);
#if ENABLE_LOG_TEST
            pr_emerg("Mock-IOMMU: SG[%d] SMC Call, Ret: %ld, PA: %pa\n", i, (long)res.a0, &pa);
#endif
        }
    }
#endif

#if ENABLE_LOG_TEST
    pr_emerg("Mock-IOMMU(DMA-OPS): map_sg dev=%s nents=%d mapped=%d\n",
             dev_name(dev), nents, ret);
#endif

    return ret;
}

static void mock_unmap_sg(struct device *dev, struct scatterlist *sgl, int nents,
                          enum dma_data_direction dir, unsigned long attrs)
{
    const struct dma_map_ops *orig = hook_get_orig(dev);
    if (orig && orig->unmap_sg)
        orig->unmap_sg(dev, sgl, nents, dir, attrs);
}

static void *mock_alloc(struct device *dev, size_t size,
                        dma_addr_t *dma_handle, gfp_t gfp, unsigned long attrs)
{
    const struct dma_map_ops *orig = hook_get_orig(dev);
    void *cpu_addr;

    dev->dma_ops = orig; 
    cpu_addr = dma_alloc_attrs(dev, size, dma_handle, gfp, attrs);
    dev->dma_ops = &mock_dma_ops; 

#if ENABLE_LOG_TEST
    if (cpu_addr)
        pr_emerg("Mock-IOMMU(DMA-OPS): alloc dev=%s size=%zu dma=0x%llx\n",
                 dev_name(dev), size, (unsigned long long)*dma_handle);
#endif

    return cpu_addr;
}

static void mock_free(struct device *dev, size_t size, void *cpu_addr,
                      dma_addr_t dma_handle, unsigned long attrs)
{
    const struct dma_map_ops *orig = hook_get_orig(dev);
    
    dev->dma_ops = orig;
    dma_free_attrs(dev, size, cpu_addr, dma_handle, attrs);
    dev->dma_ops = &mock_dma_ops;

#if ENABLE_LOG_TEST
    pr_emerg("Mock-IOMMU(DMA-OPS): free dev=%s size=%zu dma=0x%llx\n",
             dev_name(dev), size, (unsigned long long)dma_handle);
#endif
}

static const struct dma_map_ops mock_dma_ops = {
    .alloc      = mock_alloc,
    .free       = mock_free,
    .map_page   = mock_map_page,
    .unmap_page = mock_unmap_page,
    .map_sg     = mock_map_sg,
    .unmap_sg   = mock_unmap_sg,
};

/* --- C) IOMMU Domain --- */
struct mock_domain {
    struct iommu_domain domain;
    spinlock_t lock;
    struct list_head maps;
};

static int mock_attach_dev(struct iommu_domain *domain, struct device *dev)
{
    if (dev_name(dev) && strstr(dev_name(dev), "xhci-hcd")) {
        const struct dma_map_ops *orig = get_dma_ops(dev);
        hook_install(dev, orig);
        set_dma_ops(dev, &mock_dma_ops);
        pr_emerg("Mock-IOMMU: Hooked xHCI DMA ops\n");
    }
    return 0;
}

static const struct iommu_domain_ops mock_domain_ops = {
    .attach_dev = mock_attach_dev,
};

static struct iommu_domain *mock_domain_alloc(unsigned type)
{
    struct mock_domain *md = kzalloc(sizeof(*md), GFP_KERNEL);
    if (!md) return NULL;
    md->domain.type = type;
    md->domain.ops = &mock_domain_ops;
    md->domain.pgsize_bitmap = SZ_4K;
    return &md->domain;
}

static int mock_def_domain_type(struct device *dev)
{
    return IOMMU_DOMAIN_IDENTITY;
}

static struct iommu_device *mock_probe_device(struct device *dev)
{ 
    struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
    return fwspec ? global_mmu : NULL;
}

static int mock_of_xlate(struct device *dev, const struct of_phandle_args *args)
{
    return iommu_fwspec_add_ids(dev, args->args, args->args_count);
}

static const struct iommu_ops mock_iommu_ops = {
    .domain_alloc    = mock_domain_alloc,
    .probe_device    = mock_probe_device,
    .device_group    = generic_device_group,
    .of_xlate        = mock_of_xlate,
    .def_domain_type = mock_def_domain_type,
};

/* --- D) Driver Probe --- */
static int mock_iommu_probe(struct platform_device *pdev)
{
    global_mmu = devm_kzalloc(&pdev->dev, sizeof(*global_mmu), GFP_KERNEL);
    if (!global_mmu) return -ENOMEM;
    iommu_device_register(global_mmu, &mock_iommu_ops, &pdev->dev);
    iommu_device_sysfs_add(global_mmu, &pdev->dev, NULL, "mock-iommu");
    return 0;
}

static const struct of_device_id mock_iommu_of_match[] = {
    { .compatible = "my,mock-iommu" },
    { }
};

static struct platform_driver mock_iommu_driver = {
    .driver = { .name = "mock-iommu", .of_match_table = mock_iommu_of_match },
    .probe = mock_iommu_probe,
};

module_platform_driver(mock_iommu_driver);
MODULE_LICENSE("GPL");