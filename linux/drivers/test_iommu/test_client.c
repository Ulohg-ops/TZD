#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/of.h>
#include <linux/mod_devicetable.h>
#include <linux/slab.h>   // kmalloc/kfree
#include <linux/string.h> // memset (可選)

static int test_client_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    dma_addr_t dma_handle;
    void *vaddr;
    size_t size = 4096;
    int i, iterations = 1000; // 測試 1000 次
    ktime_t start, end;
    s64 total_ns;

    vaddr = kmalloc(size, GFP_KERNEL);
    if (!vaddr) return -ENOMEM;

    dev_info(dev, "--- Starting SMC Performance Test ---\n");

    start = ktime_get();
    for (i = 0; i < iterations; i++) {
        // 1. 這會進入 mock_map_pages -> 呼叫 SMC (EL1 -> EL3)
        dma_handle = dma_map_single(dev, vaddr, size, DMA_BIDIRECTIONAL);
        
        if (unlikely(dma_mapping_error(dev, dma_handle))) break;

        // 2. 這會進入 mock_unmap_pages -> (EL1)
        dma_unmap_single(dev, dma_handle, size, DMA_BIDIRECTIONAL);
    }
    end = ktime_get();

    total_ns = ktime_to_ns(ktime_sub(end, start));
    
    dev_info(dev, "Total iterations: %d\n", iterations);
    dev_info(dev, "Average Latency per Map/Unmap: %lld ns\n", div_s64(total_ns, iterations));
    dev_info(dev, "--------------------------------------\n");

    kfree(vaddr);
    return 0;
}

// static int test_client_probe(struct platform_device *pdev)
// {
//     struct device *dev = &pdev->dev;
//     dma_addr_t dma_handle;
//     void *vaddr;
//     size_t size = 4096;
//     int ret;

//     dev_info(dev, "Test Client probing (dma_map_single path)...\n");

//     // 1) 設定 DMA Mask（要檢查回傳值）
//     ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
//     if (ret) {
//         dev_err(dev, "Failed to set DMA mask: %d\n", ret);
//         return ret;
//     }

//     // 2) 配一塊一般記憶體（Streaming DMA 常見用法：kmalloc + map）
//     vaddr = kmalloc(size, GFP_KERNEL);
//     if (!vaddr) {
//         dev_err(dev, "Failed to kmalloc buffer\n");
//         return -ENOMEM;
//     }

//     // 可選：寫點資料，避免最佳化/或你想做 sync 測試
//     memset(vaddr, 0xA5, size);

//     // 3) 觸發 IOMMU map：dma_map_single()
//     //    direction 請依你的情境選，這裡用 DMA_BIDIRECTIONAL 最保守
//     dma_handle = dma_map_single(dev, vaddr, size, DMA_BIDIRECTIONAL);
//     if (dma_mapping_error(dev, dma_handle)) {
//         dev_err(dev, "dma_map_single() failed\n");
//         kfree(vaddr);
//         return -EIO;
//     }

//     dev_info(dev, "Mapped DMA address (IOVA): %pad (size=%zu)\n", &dma_handle, size);

//     /*
//      * 4) （可選）如果你之後真的要模擬 CPU<->Device 可見性：
//      *    - CPU寫完、準備給device讀：dma_sync_single_for_device()
//      *    - device寫完、準備給CPU讀：dma_sync_single_for_cpu()
//      *
//      * 這裡先不做也沒關係，你要的目標是確定 map/unmap 會被呼叫。
//      */

//     // 5) 觸發 IOMMU unmap：dma_unmap_single()
//     dma_unmap_single(dev, dma_handle, size, DMA_BIDIRECTIONAL);
//     dev_info(dev, "Unmapped DMA address (IOVA): %pad\n", &dma_handle);

//     // 6) 釋放 kmalloc 的 buffer
//     kfree(vaddr);

//     return 0;
// }

static const struct of_device_id test_client_of_match[] = {
    { .compatible = "my,secure-dma-test" },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, test_client_of_match);

static struct platform_driver test_client_driver = {
    .driver = {
        .name = "test-client",
        .of_match_table = test_client_of_match,
    },
    .probe = test_client_probe,
};

module_platform_driver(test_client_driver);
MODULE_LICENSE("GPL");
