/*
 * Copyright (c) 2023 Antmicro <www.antmicro.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT ambiq_spi

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(spi_ambiq);

#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/spi/rtio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/policy.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/cache.h>

#include <stdlib.h>
#include <errno.h>
#include "spi_context.h"

#include <soc.h>

struct spi_ambiq_config {
	uint32_t base;
	int size;
	int inst_idx;
	uint32_t clock_freq;
	const struct pinctrl_dev_config *pcfg;
	void (*irq_config_func)(void);
};

struct spi_ambiq_data {
	struct spi_context ctx;
	am_hal_iom_config_t iom_cfg;
	void *iom_handler;
	bool cont;
	bool pm_policy_state_on;
	bool dma_mode;
};

typedef void (*spi_context_update_trx)(struct spi_context *ctx, uint8_t dfs, uint32_t len);

#define SPI_WORD_SIZE 8

static void spi_ambiq_pm_policy_state_lock_get(const struct device *dev)
{
	if (IS_ENABLED(CONFIG_PM)) {
		struct spi_ambiq_data *data = dev->data;

		if (!data->pm_policy_state_on) {
			data->pm_policy_state_on = true;
			pm_policy_state_lock_get(PM_STATE_SUSPEND_TO_RAM, PM_ALL_SUBSTATES);
			pm_device_runtime_get(dev);
		}
	}
}

static void spi_ambiq_pm_policy_state_lock_put(const struct device *dev)
{
	if (IS_ENABLED(CONFIG_PM)) {
		struct spi_ambiq_data *data = dev->data;

		if (data->pm_policy_state_on) {
			data->pm_policy_state_on = false;
			pm_device_runtime_put(dev);
			pm_policy_state_lock_put(PM_STATE_SUSPEND_TO_RAM, PM_ALL_SUBSTATES);
		}
	}
}

static void spi_ambiq_callback(void *callback_ctxt, uint32_t status)
{
	const struct device *dev = callback_ctxt;
	struct spi_ambiq_data *data = dev->data;
	struct spi_context *ctx = &data->ctx;

	/* de-assert cs until transfer finished and no need to hold cs */
	if (!data->cont) {
		spi_context_cs_control(ctx, false);
	}
	spi_context_complete(ctx, dev, (status == AM_HAL_STATUS_SUCCESS) ? 0 : -EIO);
}

static void spi_ambiq_reset(const struct device *dev)
{
	struct spi_ambiq_data *data = dev->data;
	struct spi_context *ctx = &data->ctx;

	/* cancel timed out transaction */
	am_hal_iom_disable(data->iom_handler);
	/* NULL config to trigger reconfigure on next xfer */
	ctx->config = NULL;
	spi_context_cs_control(ctx, false);
	/* signal any thread waiting on sync semaphore */
	spi_context_complete(ctx, dev, -ETIMEDOUT);
	/* clean up for next xfer */
	k_sem_reset(&ctx->sync);
}

static void spi_ambiq_isr(const struct device *dev)
{
	uint32_t ui32Status;
	struct spi_ambiq_data *data = dev->data;

	am_hal_iom_interrupt_status_get(data->iom_handler, false, &ui32Status);
	am_hal_iom_interrupt_clear(data->iom_handler, ui32Status);
	am_hal_iom_interrupt_service(data->iom_handler, ui32Status);
}

static int spi_config(const struct device *dev, const struct spi_config *config)
{
	struct spi_ambiq_data *data = dev->data;
	const struct spi_ambiq_config *cfg = dev->config;
	struct spi_context *ctx = &(data->ctx);

	int ret = 0;

	if (spi_context_configured(ctx, config)) {
		/* Already configured. No need to do it again. */
		return 0;
	}

	if (SPI_WORD_SIZE_GET(config->operation) != SPI_WORD_SIZE) {
		LOG_ERR("Word size must be %d", SPI_WORD_SIZE);
		return -ENOTSUP;
	}

	if ((config->operation & SPI_LINES_MASK) != SPI_LINES_SINGLE) {
		LOG_ERR("Only supports single mode");
		return -ENOTSUP;
	}

	if (config->operation & SPI_LOCK_ON) {
		LOG_ERR("Lock On not supported");
		return -ENOTSUP;
	}

	if (config->operation & SPI_TRANSFER_LSB) {
		LOG_ERR("LSB first not supported");
		return -ENOTSUP;
	}

	if (config->operation & SPI_MODE_CPOL) {
		if (config->operation & SPI_MODE_CPHA) {
			data->iom_cfg.eSpiMode = AM_HAL_IOM_SPI_MODE_3;
		} else {
			data->iom_cfg.eSpiMode = AM_HAL_IOM_SPI_MODE_2;
		}
	} else {
		if (config->operation & SPI_MODE_CPHA) {
			data->iom_cfg.eSpiMode = AM_HAL_IOM_SPI_MODE_1;
		} else {
			data->iom_cfg.eSpiMode = AM_HAL_IOM_SPI_MODE_0;
		}
	}

	if (config->operation & SPI_OP_MODE_SLAVE) {
		LOG_ERR("Device mode not supported");
		return -ENOTSUP;
	}
	if (config->operation & SPI_MODE_LOOP) {
		LOG_ERR("Loopback mode not supported");
		return -ENOTSUP;
	}

	if (cfg->clock_freq > AM_HAL_IOM_MAX_FREQ) {
		LOG_ERR("Clock frequency too high");
		return -ENOTSUP;
	}

	/* Select slower of two: SPI bus frequency for SPI device or SPI controller clock frequency
	 */
	data->iom_cfg.ui32ClockFreq =
		(config->frequency ? MIN(config->frequency, cfg->clock_freq) : cfg->clock_freq);
	ctx->config = config;

	/* Disable IOM instance as it cannot be configured when enabled*/
	ret = am_hal_iom_disable(data->iom_handler);

	ret = am_hal_iom_configure(data->iom_handler, &data->iom_cfg);

	ret = am_hal_iom_enable(data->iom_handler);

	return ret;
}

static int spi_ambiq_xfer_half_duplex(const struct device *dev, am_hal_iom_dir_e dir)
{
	am_hal_iom_transfer_t trans = {0};
	struct spi_ambiq_data *data = dev->data;
	struct spi_context *ctx = &data->ctx;
	bool is_last = false;
	uint32_t rem_num, cur_num = 0;
	int ret = 0;
	spi_context_update_trx ctx_update;

	if (dir == AM_HAL_IOM_FULLDUPLEX) {
		return -EINVAL;
	} else if (dir == AM_HAL_IOM_RX) {
		trans.eDirection = AM_HAL_IOM_RX;
		ctx_update = spi_context_update_rx;
	} else {
		trans.eDirection = AM_HAL_IOM_TX;
		ctx_update = spi_context_update_tx;
	}
	if (dir == AM_HAL_IOM_RX) {
		rem_num = ctx->rx_len;
	} else {
		rem_num = ctx->tx_len;
	}
	while (rem_num) {
		cur_num = (rem_num > AM_HAL_IOM_MAX_TXNSIZE_SPI) ? AM_HAL_IOM_MAX_TXNSIZE_SPI
								 : rem_num;
		trans.ui32NumBytes = cur_num;
		trans.pui32TxBuffer = (uint32_t *)ctx->tx_buf;
		trans.pui32RxBuffer = (uint32_t *)ctx->rx_buf;
		ctx_update(ctx, 1, cur_num);
		if ((!spi_context_tx_buf_on(ctx)) && (!spi_context_rx_on(ctx))) {
			is_last = true;
		}
		if (data->dma_mode) {
#if CONFIG_SPI_AMBIQ_HANDLE_CACHE
			/* Clean Dcache before DMA write */
			if ((trans.eDirection == AM_HAL_IOM_TX) && (trans.pui32TxBuffer) &&
			    (!buf_in_nocache((uintptr_t)trans.pui32TxBuffer, trans.ui32NumBytes))) {
				sys_cache_data_flush_range((void *)trans.pui32TxBuffer,
							   trans.ui32NumBytes);
			}
#endif /* CONFIG_SPI_AMBIQ_HANDLE_CACHE */
			if (AM_HAL_STATUS_SUCCESS !=
			    am_hal_iom_nonblocking_transfer(
				    data->iom_handler, &trans,
				    ((is_last == true) ? spi_ambiq_callback : NULL), (void *)dev)) {
				return -EIO;
			}
			if (is_last) {
				ret = spi_context_wait_for_completion(ctx);
#if CONFIG_SPI_AMBIQ_HANDLE_CACHE
				/* Invalidate Dcache after DMA read */
				if ((trans.eDirection == AM_HAL_IOM_RX) && (trans.pui32RxBuffer) &&
				    (!buf_in_nocache((uintptr_t)trans.pui32RxBuffer,
						     trans.ui32NumBytes))) {
					sys_cache_data_invd_range((void *)trans.pui32RxBuffer,
								  trans.ui32NumBytes);
				}
#endif /* CONFIG_SPI_AMBIQ_HANDLE_CACHE */
			}
		} else {
			ret = am_hal_iom_blocking_transfer(data->iom_handler, &trans);
		}
		rem_num -= cur_num;
		if (ret != 0) {
			return -EIO;
		}
	}

	return 0;
}

static int spi_ambiq_xfer_full_duplex(const struct device *dev)
{
	am_hal_iom_transfer_t trans = {0};
	struct spi_ambiq_data *data = dev->data;
	struct spi_context *ctx = &data->ctx;
	bool trx_once = (ctx->tx_len == ctx->rx_len);
	int ret = 0;

	/* Tx and Rx length must be the same for am_hal_iom_spi_blocking_fullduplex */
	trans.eDirection = AM_HAL_IOM_FULLDUPLEX;
	trans.ui32NumBytes = MIN(ctx->rx_len, ctx->tx_len);
	trans.pui32RxBuffer = (uint32_t *)ctx->rx_buf;
	trans.pui32TxBuffer = (uint32_t *)ctx->tx_buf;
	spi_context_update_tx(ctx, 1, trans.ui32NumBytes);
	spi_context_update_rx(ctx, 1, trans.ui32NumBytes);

	ret = am_hal_iom_spi_blocking_fullduplex(data->iom_handler, &trans);
	if (ret != 0) {
		return -EIO;
	}

	/* Transfer the remaining bytes */
	if (!trx_once) {
		spi_context_update_trx ctx_update;

		if (ctx->tx_len) {
			trans.eDirection = AM_HAL_IOM_TX;
			trans.ui32NumBytes = ctx->tx_len;
			trans.pui32TxBuffer = (uint32_t *)ctx->tx_buf;
			ctx_update = spi_context_update_tx;
		} else {
			trans.eDirection = AM_HAL_IOM_RX;
			trans.ui32NumBytes = ctx->rx_len;
			trans.pui32RxBuffer = (uint32_t *)ctx->rx_buf;
			ctx_update = spi_context_update_rx;
		}
		ret = am_hal_iom_blocking_transfer(data->iom_handler, &trans);
		ctx_update(ctx, 1, trans.ui32NumBytes);
		if (ret != 0) {
			return -EIO;
		}
	}

	return 0;
}

static int spi_ambiq_xfer(const struct device *dev, const struct spi_config *config)
{
	struct spi_ambiq_data *data = dev->data;
	struct spi_context *ctx = &data->ctx;
	int ret = 0;
	data->cont = (config->operation & SPI_HOLD_ON_CS) ? true : false;

	spi_context_cs_control(ctx, true);

	while (1) {
		if (spi_context_tx_buf_on(ctx) && spi_context_rx_buf_on(ctx)) {
			if (ctx->rx_buf == ctx->tx_buf) {
				spi_context_update_rx(ctx, 1, ctx->rx_len);
			} else if (!(config->operation & SPI_HALF_DUPLEX)) {
				ret = spi_ambiq_xfer_full_duplex(dev);
				if (ret != 0) {
					spi_ambiq_reset(dev);
					LOG_ERR("SPI full-duplex comm error: %d", ret);
					return ret;
				}
			}
		}
		if (spi_context_tx_on(ctx)) {
			if (ctx->tx_buf == NULL) {
				spi_context_update_tx(ctx, 1, ctx->tx_len);
			} else {
				ret = spi_ambiq_xfer_half_duplex(dev, AM_HAL_IOM_TX);
				if (ret != 0) {
					spi_ambiq_reset(dev);
					LOG_ERR("SPI TX comm error: %d", ret);
					return ret;
				}
			}
		} else if (spi_context_rx_on(ctx)) {
			if (ctx->rx_buf == NULL) {
				spi_context_update_rx(ctx, 1, ctx->rx_len);
			} else {
				ret = spi_ambiq_xfer_half_duplex(dev, AM_HAL_IOM_RX);
				if (ret != 0) {
					spi_ambiq_reset(dev);
					LOG_ERR("SPI Rx comm error: %d", ret);
					return ret;
				}
			}
		} else {
			break;
		}
	}

	if ((!data->dma_mode) && (!data->cont)) {
		spi_context_cs_control(ctx, false);
		spi_context_complete(ctx, dev, ret);
	}

	return ret;
}

static int spi_ambiq_transceive(const struct device *dev, const struct spi_config *config,
				const struct spi_buf_set *tx_bufs,
				const struct spi_buf_set *rx_bufs)
{
	struct spi_ambiq_data *data = dev->data;
	int ret = 0;

	if (!tx_bufs && !rx_bufs) {
		return 0;
	}

	/* context setup */
	spi_context_lock(&data->ctx, false, NULL, NULL, config);

	spi_ambiq_pm_policy_state_lock_get(dev);

	ret = spi_config(dev, config);

	if (ret) {
		LOG_ERR("spi_config failed: %d", ret);
		goto xfer_end;
	}

	spi_context_buffers_setup(&data->ctx, tx_bufs, rx_bufs, 1);

	ret = spi_ambiq_xfer(dev, config);

xfer_end:
	spi_ambiq_pm_policy_state_lock_put(dev);

	spi_context_release(&data->ctx, ret);

	return ret;
}

static int spi_ambiq_release(const struct device *dev, const struct spi_config *config)
{
	struct spi_ambiq_data *data = dev->data;
	am_hal_iom_status_t iom_status;

	am_hal_iom_status_get(data->iom_handler, &iom_status);

	if ((iom_status.bStatIdle != IOM0_STATUS_IDLEST_IDLE) ||
	    (iom_status.bStatCmdAct == IOM0_STATUS_CMDACT_ACTIVE) ||
	    (iom_status.ui32NumPendTransactions)) {
		return -EBUSY;
	}

	spi_context_unlock_unconditionally(&data->ctx);

	return 0;
}

static DEVICE_API(spi, spi_ambiq_driver_api) = {
	.transceive = spi_ambiq_transceive,
#ifdef CONFIG_SPI_RTIO
	.iodev_submit = spi_rtio_iodev_default_submit,
#endif
	.release = spi_ambiq_release,
};

static int spi_ambiq_init(const struct device *dev)
{
	struct spi_ambiq_data *data = dev->data;
	const struct spi_ambiq_config *cfg = dev->config;
	int ret = 0;

	if (AM_HAL_STATUS_SUCCESS != am_hal_iom_initialize(cfg->inst_idx, &data->iom_handler)) {
		LOG_ERR("Fail to initialize SPI\n");
		return -ENXIO;
	}

	ret = am_hal_iom_power_ctrl(data->iom_handler, AM_HAL_SYSCTRL_WAKE, false);

	ret |= pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
	ret |= spi_context_cs_configure_all(&data->ctx);
	if (ret < 0) {
		LOG_ERR("Fail to config SPI pins\n");
		goto end;
	}

	if (data->dma_mode) {
		am_hal_iom_interrupt_clear(data->iom_handler,
					   AM_HAL_IOM_INT_CQUPD | AM_HAL_IOM_INT_ERR);
		am_hal_iom_interrupt_enable(data->iom_handler,
					    AM_HAL_IOM_INT_CQUPD | AM_HAL_IOM_INT_ERR);
		cfg->irq_config_func();
	}
end:
	if (ret < 0) {
		am_hal_iom_uninitialize(data->iom_handler);
	} else {
		spi_context_unlock_unconditionally(&data->ctx);
	}
	return ret;
}

#ifdef CONFIG_PM_DEVICE
static int spi_ambiq_pm_action(const struct device *dev, enum pm_device_action action)
{
	const struct spi_ambiq_config *cfg = dev->config;
	struct spi_ambiq_data *data = dev->data;
	int err;
	am_hal_sysctrl_power_state_e status;

	switch (action) {
	case PM_DEVICE_ACTION_RESUME:
		/* Set pins to active state */
		err = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
		if (err < 0) {
			return err;
		}
		status = AM_HAL_SYSCTRL_WAKE;
		break;
	case PM_DEVICE_ACTION_SUSPEND:
		/* Move pins to sleep state */
		err = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_SLEEP);
		if ((err < 0) && (err != -ENOENT)) {
			/*
			 * If returning -ENOENT, no pins where defined for sleep mode :
			 * Do not output on console (might sleep already) when going to
			 * sleep,
			 * "SPI pinctrl sleep state not available"
			 * and don't block PM suspend.
			 * Else return the error.
			 */
			return err;
		}
		status = AM_HAL_SYSCTRL_DEEPSLEEP;
		break;
	default:
		return -ENOTSUP;
	}

	err = am_hal_iom_power_ctrl(data->iom_handler, status, true);

	if (err != AM_HAL_STATUS_SUCCESS) {
		LOG_ERR("am_hal_iom_power_ctrl failed: %d", err);
		return -EPERM;
	} else {
		return 0;
	}
}
#endif /* CONFIG_PM_DEVICE */

#define IOM_HAL_CFG(n, cmdq, cmdq_size)                                                            \
	{                                                                                          \
		.eInterfaceMode     = AM_HAL_IOM_SPI_MODE,                                         \
		.ui32ClockFreq      = AM_HAL_IOM_100KHZ,                                           \
		.pNBTxnBuf          = cmdq,                                                        \
		.ui32NBTxnBufLength = cmdq_size,                                                   \
	}

#define AMBIQ_SPI_INIT(n)                                                                          \
	BUILD_ASSERT(DT_CHILD_NUM_STATUS_OKAY(DT_INST_PARENT(n)) == 1,                             \
		     "Too many children for IOM, either SPI or I2C should be enabled!");           \
	PINCTRL_DT_INST_DEFINE(n);                                                                 \
	static void spi_irq_config_func_##n(void)                                                  \
	{                                                                                          \
		IRQ_CONNECT(DT_IRQN(DT_INST_PARENT(n)), DT_IRQ(DT_INST_PARENT(n), priority),       \
			    spi_ambiq_isr, DEVICE_DT_INST_GET(n), 0);                              \
		irq_enable(DT_IRQN(DT_INST_PARENT(n)));                                            \
	};                                                                                         \
	IF_ENABLED(DT_PROP(DT_INST_PARENT(n), dma_mode),                                           \
	(static uint32_t spi_ambiq_cmdq##n[DT_PROP_OR(DT_INST_PARENT(n), cmdq_buffer_size, 1024)]  \
	 __attribute__((section(DT_PROP_OR(DT_INST_PARENT(n),                                      \
					  cmdq_buffer_location, ".nocache"))));)                   \
	)                                                                                          \
	static struct spi_ambiq_data spi_ambiq_data##n = {                                         \
		.iom_cfg = IOM_HAL_CFG(                                                            \
			n, COND_CODE_1(DT_PROP(DT_INST_PARENT(n), dma_mode), (spi_ambiq_cmdq##n),  \
									    (NULL)),               \
				COND_CODE_1(DT_PROP(DT_INST_PARENT(n), dma_mode),                 \
					(DT_INST_PROP_OR(n, cmdq_buffer_size, 1024)), (0))),   \
		.dma_mode = DT_PROP(DT_INST_PARENT(n), dma_mode),                         \
		SPI_CONTEXT_INIT_LOCK(spi_ambiq_data##n, ctx),                                     \
		SPI_CONTEXT_INIT_SYNC(spi_ambiq_data##n, ctx),                                     \
		SPI_CONTEXT_CS_GPIOS_INITIALIZE(DT_DRV_INST(n), ctx)};                             \
	static const struct spi_ambiq_config spi_ambiq_config##n = {                               \
		.base = DT_REG_ADDR(DT_INST_PARENT(n)),                                            \
		.size = DT_REG_SIZE(DT_INST_PARENT(n)),                                            \
		.inst_idx =                                                                        \
			(DT_REG_ADDR(DT_INST_PARENT(n)) - IOM0_BASE) / (IOM1_BASE - IOM0_BASE),    \
		.clock_freq = DT_INST_PROP(n, clock_frequency),                                    \
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                         \
		.irq_config_func = spi_irq_config_func_##n};                                       \
	PM_DEVICE_DT_INST_DEFINE(n, spi_ambiq_pm_action);                                          \
	SPI_DEVICE_DT_INST_DEFINE(n, spi_ambiq_init, PM_DEVICE_DT_INST_GET(n), &spi_ambiq_data##n, \
				  &spi_ambiq_config##n, POST_KERNEL, CONFIG_SPI_INIT_PRIORITY,     \
				  &spi_ambiq_driver_api);

DT_INST_FOREACH_STATUS_OKAY(AMBIQ_SPI_INIT)
