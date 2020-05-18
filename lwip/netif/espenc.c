#include <lwip/ip_addr.h>
#include <lwip/dhcp.h>
#include "netif/espenc.h"
#include "netif/driver/spi.h"
#include "gpio.h"
#include "mem.h"

#if LWIP_HAVE_ENC28J60IF

struct netif enc_netif;

typedef enum {
        SIG_DO_NOTHING = 0,
        SIG_NETIF_POLL,
        SIG_ENC_SCOOP
} USER_SIGNALS;

#define swint_TaskPrio        2
#define swint_TaskQueueLen    2
os_event_t swint_TaskQueue[swint_TaskQueueLen];
static void swint_Task(os_event_t *events);

static uint8_t Enc28j60Bank;
static uint16_t NextPacketPtr;
static volatile bool inint = false;
static volatile bool handling_int = false;

void ICACHE_FLASH_ATTR chipEnable() {
        // Force CS pin low
        while(spi_busy(HSPI));
        PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTDO_U, FUNC_GPIO15);
        GPIO_OUTPUT_SET(ESP_CS, 0);
}

void ICACHE_FLASH_ATTR chipDisable() {
        // Return to default CS function
        while(spi_busy(HSPI));
        PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTDO_U, 2); //GPIO15 is HSPI CS pin (Chip Select / Slave Select)
}

uint8_t readOp(uint8_t op, uint8_t addr) {
        while(spi_busy(HSPI));
        if(addr & 0x80)
                return(uint8_t) spi_transaction(HSPI, 3, op >> 5, 5, addr, 0, 0, 16, 0) & 0xff; // Ignore dummy first byte
        else
                return(uint8_t) spi_transaction(HSPI, 3, op >> 5, 5, addr, 0, 0, 8, 0);
}

void writeOp(uint8_t op, uint8_t addr, uint8_t data) {
        while(spi_busy(HSPI));
        spi_transaction(HSPI, 3, op >> 5, 5, addr, 8, data, 0, 0);
}

static void SetBank(uint8_t address) {
        while(spi_busy(HSPI));
        if((address & BANK_MASK) != Enc28j60Bank) {
                writeOp(ENC28J60_BIT_FIELD_CLR, ECON1, ECON1_BSEL1 | ECON1_BSEL0);
                Enc28j60Bank = address & BANK_MASK;
                writeOp(ENC28J60_BIT_FIELD_SET, ECON1, Enc28j60Bank >> 5);
        }
}

static uint8_t readRegByte(uint8_t address) {
        SetBank(address);
        return readOp(ENC28J60_READ_CTRL_REG, address);
}

static uint16_t readReg(uint8_t address) {
        return readRegByte(address) + (readRegByte(address + 1) << 8);
}

static void writeRegByte(uint8_t address, uint8_t data) {
        SetBank(address);
        writeOp(ENC28J60_WRITE_CTRL_REG, address, data);
}

static void writeReg(uint8_t address, uint16_t data) {
        writeRegByte(address, data);
        writeRegByte(address + 1, data >> 8);
}

static uint16_t readPhyByte(uint8_t address) {
        writeRegByte(MIREGADR, address);
        writeRegByte(MICMD, MICMD_MIIRD);
        while(readRegByte(MISTAT) & MISTAT_BUSY);
        writeRegByte(MICMD, 0x00);
        return readRegByte(MIRD + 1);
}

static void writePhy(uint8_t address, uint16_t data) {
        writeRegByte(MIREGADR, address);
        writeReg(MIWR, data);
        while(readRegByte(MISTAT) & MISTAT_BUSY);
}

static void readBuf(uint16_t len, uint8_t* data) {
        log("readBuf()");
        if(len != 0) {
                chipEnable();
                spi_transaction(HSPI, 8, ENC28J60_READ_BUF_MEM, 0, 0, 0, 0, 0, 0);
                while(len--) {
                        uint8_t nextbyte;
                        while(spi_busy(HSPI)); //wait for SPI transaction to complete
                        nextbyte = spi_transaction(HSPI, 0, 0, 0, 0, 0, 0, 8, 0);
                        *data++ = nextbyte;
                };
                chipDisable();
        }
}

static void writeBuf(uint16_t len, const uint8_t* data) {
        if(len != 0) {
                chipEnable();
                spi_transaction(HSPI, 8, ENC28J60_WRITE_BUF_MEM, 0, 0, 8, 0, 0, 0);
                while(len--) {
                        uint8_t nextbyte = *data++;
                        while(spi_busy(HSPI)); //wait for SPI transaction to complete
                        spi_transaction(HSPI, 0, 0, 0, 0, 8, nextbyte, 0, 0);
                };
                chipDisable();
        }
}

err_t ICACHE_FLASH_ATTR enc28j60_link_output(struct netif *netif, struct pbuf *p) {
        // Is this called from a critical section?
        if(inint) {
                if(handling_int) {
                        os_printf("OOPS handling int!\r\n");
                }
        }
        gpio_pin_intr_state_set(GPIO_ID_PIN(ESP_INT), GPIO_PIN_INTR_DISABLE);
        uint16_t len = p->tot_len;

        log("output, tot_len: %d", p->tot_len);
        uint8_t isUp = (readPhyByte(PHSTAT2) >> 2) & 1;
        log("link is up: %d", isUp);
        log("pktcnt: %d", readRegByte(EPKTCNT));

        SetBank(ECON1);
        writeOp(ENC28J60_BIT_FIELD_SET, ECON1, ECON1_TXRST);
        writeOp(ENC28J60_BIT_FIELD_CLR, ECON1, ECON1_TXRST);
        SetBank(EIR);
        writeOp(ENC28J60_BIT_FIELD_CLR, EIR, EIR_TXERIF | EIR_TXIF);

        writeReg(EWRPT, TXSTART_INIT);
        writeReg(ETXND, TXSTART_INIT + len);
        //writeOp(ENC28J60_WRITE_BUF_MEM, 0, 0x00);
        uint8_t* buffer = (uint8_t*) os_malloc(len);
        pbuf_copy_partial(p, buffer, p->tot_len, 0);
        writeBuf(len, buffer);
        os_free(buffer);

        SetBank(EIR);
        writeOp(ENC28J60_BIT_FIELD_CLR, EIR, EIR_TXERIF | EIR_TXIF);
        log("before transmission: %02x", readRegByte(EIR));
        SetBank(ECON1);
        writeOp(ENC28J60_BIT_FIELD_SET, ECON1, ECON1_TXRTS);

        uint16_t count = 0;
        uint16_t eir = 0;
        /*
         * Possible Problem: This might only work at 4MHz SPI
         * so we delay a fixed amount.
         */
        while(((eir = readRegByte(EIR)) & (EIR_TXIF | EIR_TXERIF)) == 0 && ++count < 1000U) {
                os_delay_us(1);
        }

        if(!(eir & EIR_TXERIF) && count < 1000U) {
                // no error; start new transmission
                log("transmission success");
                SetBank(ECON1);
                writeOp(ENC28J60_BIT_FIELD_CLR, ECON1, ECON1_TXRTS);
                SetBank(EIR);
                writeOp(ENC28J60_BIT_FIELD_CLR, EIR, EIR_TXIF);
        } else {
                log("transmission failed (%d - %02x)", count, eir);
                // wait - the longer the packet, the longer the wait
                os_delay_us(2 * len);

                SetBank(ECON1);
                writeOp(ENC28J60_BIT_FIELD_CLR, ECON1, ECON1_TXRST);
                writeOp(ENC28J60_BIT_FIELD_CLR, ECON1, ECON1_TXRST | ECON1_TXRTS);
                SetBank(EIR);
                writeOp(ENC28J60_BIT_FIELD_CLR, EIR, EIR_TXIF);
                writeOp(ENC28J60_BIT_FIELD_CLR, EIR, EIR_TXERIF);
        }

        gpio_pin_intr_state_set(GPIO_ID_PIN(ESP_INT), GPIO_PIN_INTR_LOLEVEL);
        return 0;
}

static uint32_t interrupt_reg = 0;

void enc28j60_handle_packets(void) {
        log("reading ptr: %04x", NextPacketPtr);
        writeReg(ERDPT, NextPacketPtr);
        uint16_t packetLen = 0;
        uint16_t rxStatus = 0;

        readBuf(2, (uint8_t*) & NextPacketPtr);
        readBuf(2, (uint8_t*) & packetLen);
        readBuf(2, (uint8_t*) & rxStatus);

        // Ignore packet checksum TODO
        packetLen -= 4;

        log("next ptr: %04x", NextPacketPtr);
        log("packet len: %d (%x)", packetLen, packetLen);
        log("rx status: %02x", rxStatus);

        if(rxStatus & 0x80 == 0) {
                log("RECEIVE FAILED");
        } else {
                uint16_t len = packetLen;
                struct pbuf* p = pbuf_alloc(PBUF_LINK, len, PBUF_RAM);
                if(p != 0) {
                        uint8_t* data;
                        struct pbuf* q;
                        struct pbuf* last;

                        for(q = p; q != 0; q = q->next) {
                                data = q->payload;
                                len = q->len;

                                log("reading %d to %x", len, data);
                                readBuf(len, data);
                        }

#if !ENC_SW_INTERRUPT
                        /*
                         *  I think we should ALWAYS use SWI.
                         *  not doing so crashes horribly under load
                         */
                        log("packet received, passing to netif->input");
                        enc_netif.input(p, &enc_netif);
#else
                        /* let last point to the last pbuf in chain r */
                        for(last = p; last->next != NULL; last = last->next);
                        // os_printf("ENQUEUE %d at %x\r\n", p->tot_len, p);
                        SYS_ARCH_PROTECT(lev);
                        if(enc_netif.loop_first != NULL) {
                                LWIP_ASSERT("if first != NULL, last must also be != NULL", enc_netif.loop_last != NULL);
                                enc_netif.loop_last->next = p;
                                enc_netif.loop_last = last;
                        } else {
                                enc_netif.loop_first = p;
                                enc_netif.loop_last = last;
                        }
                        SYS_ARCH_UNPROTECT(lev);

#if LWIP_NETIF_LOOPBACK_MULTITHREADING
                        // For multithreading environment, schedule a call to netif_poll
                        tcpip_callback((tcpip_callback_fn) netif_poll, &enc_netif);
#else
                        system_os_post(swint_TaskPrio, SIG_NETIF_POLL, (ETSParam) & enc_netif);

#endif /* LWIP_NETIF_LOOPBACK_MULTITHREADING */
#endif
                } else {
                        log("pbuf_alloc failed!");
                }
        }

        SetBank(ECON2);
        writeOp(ENC28J60_BIT_FIELD_SET, ECON2, ECON2_PKTDEC);

        writeReg(ERXRDPT, NextPacketPtr);
}

void enc_scoop_packets(void) {
        handling_int = true;
        while(readRegByte(EPKTCNT) > 0)
                enc28j60_handle_packets();

        SetBank(EIR);
        writeOp(ENC28J60_BIT_FIELD_CLR, EIR, EIR_PKTIF);
        // ready for next IRQ
        //log1("INT F");
        handling_int = false;
        inint = false;
        gpio_pin_intr_state_set(GPIO_ID_PIN(ESP_INT), GPIO_PIN_INTR_LOLEVEL);
}

// SW Interrupt handler

static void ICACHE_FLASH_ATTR swint_Task(os_event_t *events) {
        //os_printf("Sig: %d\r\n", events->sig);

        switch(events->sig) {
                case SIG_ENC_SCOOP:
                        enc_scoop_packets();
                        break;

                case SIG_NETIF_POLL:
                {
                        struct netif *netif = (struct netif *) events->par;
                        netif_poll(netif);
                        break;
                }
                case SIG_DO_NOTHING:
                default:
                        // Intentionally ignoring other signals
                        os_printf("Spurious Signal received\r\n");
                        break;
        }
}

void interrupt_handler(void *arg) {
        inint = true;
        //log1("INT T");
        uint32 gpio_status = GPIO_REG_READ(GPIO_STATUS_ADDRESS);

        //ETS_GPIO_INTR_DISABLE();
        //GPIO_REG_WRITE(GPIO_STATUS_W1TC_ADDRESS, GPIO_REG_READ(GPIO_STATUS_ADDRESS));

        if(1 << ESP_INT & gpio_status) {
                gpio_pin_intr_state_set(GPIO_ID_PIN(ESP_INT), GPIO_PIN_INTR_DISABLE);

                uint8_t interrupt = readRegByte(EIR);
                uint8_t pktCnt = readRegByte(EPKTCNT);

                log("\r\n *** INTERRUPT (%02X / %d) ***", interrupt, pktCnt);

                if(pktCnt > 0 && (interrupt & EIR_PKTIF)) {
                        log("pktCnt > 0");
                        // ACK interrupt
                        GPIO_REG_WRITE(GPIO_STATUS_W1TC_ADDRESS, gpio_status & BIT(ESP_INT));

                        system_os_post(swint_TaskPrio, SIG_ENC_SCOOP, 0);
                }
        } else {
                // Spurious IRQ?
                os_printf("Spurious IRQ!!!");
                GPIO_REG_WRITE(GPIO_STATUS_W1TC_ADDRESS, gpio_status & BIT(ESP_INT));
        }

}

// http://lwip.wikia.com/wiki/Writing_a_device_driver

err_t ICACHE_FLASH_ATTR enc28j60_init(struct netif *netif) {
        log("initializing");
        netif->linkoutput = enc28j60_link_output;
        netif->name[0] = 'e';
        netif->name[1] = 'n';
        netif->mtu = 1500;
        netif->hwaddr_len = 6;

        netif->output = etharp_output;
        netif->flags |= NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;

        log("initializing hardware");
        spi_init(HSPI);
        spi_mode(HSPI, 0, 0);

        writeOp(ENC28J60_SOFT_RESET, 0, ENC28J60_SOFT_RESET);
        os_delay_us(2000); // errata B7/2
        uint8_t estat;
        while(!(estat = readOp(ENC28J60_READ_CTRL_REG, ESTAT)) & ESTAT_CLKRDY) {
                log("estat: %02x", estat);
        }
        NextPacketPtr = RXSTART_INIT;
        writeReg(ERXST, RXSTART_INIT);
        writeReg(ERXRDPT, RXSTART_INIT);
        writeReg(ERXND, RXSTOP_INIT);
        writeReg(ETXST, TXSTART_INIT);
        writeReg(ETXND, TXSTOP_INIT);

        writeRegByte(ERXFCON, ERXFCON_UCEN | ERXFCON_CRCEN | ERXFCON_PMEN | ERXFCON_BCEN);
        writeReg(EPMM0, 0x303f);
        writeReg(EPMCS, 0xf7f9);
        writeRegByte(MACON1, MACON1_MARXEN | MACON1_TXPAUS | MACON1_RXPAUS);
        writeRegByte(MACON2, 0x00);
        writeOp(ENC28J60_BIT_FIELD_SET, MACON3,
                MACON3_PADCFG0 | MACON3_TXCRCEN | MACON3_FRMLNEN);
        writeReg(MAIPG, 0x0C12);
        writeRegByte(MABBIPG, 0x12);
        writeReg(MAMXFL, MAX_FRAMELEN);
        writeRegByte(MAADR5, netif->hwaddr[0]);
        writeRegByte(MAADR4, netif->hwaddr[1]);
        writeRegByte(MAADR3, netif->hwaddr[2]);
        writeRegByte(MAADR2, netif->hwaddr[3]);
        writeRegByte(MAADR1, netif->hwaddr[4]);
        writeRegByte(MAADR0, netif->hwaddr[5]);

        // Force soft reset PHY.
        // Sometimes the SPI reset does not reset it,
        // or it is not actually ready yet.
        // I'm unsure which is the actual case, but this solves
        // a problem that occurs for me, where the PHY never seems
        // to be able to receive packets. --AJK
        writePhy(PHCON1, PHCON1_PRST);
        while(!(estat = readPhyByte(PHCON1)) & PHCON1_PRST) {
                log("Phy reset stat: %02x", estat);
        }
        writePhy(PHCON2, PHCON2_HDLDIS);
        SetBank(EIE);
        writeOp(ENC28J60_BIT_FIELD_SET, EIE, EIE_INTIE | EIE_PKTIE);

        SetBank(EIR);
        writeOp(ENC28J60_BIT_FIELD_CLR, EIR, EIR_PKTIF);

        SetBank(ECON1);
        writeOp(ENC28J60_BIT_FIELD_SET, ECON1, ECON1_RXEN);

        uint8_t rev = readRegByte(EREVID);
        // microchip forgot to step the number on the silcon when they
        // released the revision B7. 6 is now rev B7. We still have
        // to see what they do when they release B8. At the moment
        // there is no B8 out yet
        if(rev > 5) ++rev;
        log("hardware ready, rev: %d", rev);

        // You should __ALWAYS__ start ISR's __LAST__
        ETS_GPIO_INTR_ATTACH(interrupt_handler, &interrupt_reg);
        ETS_GPIO_INTR_ENABLE();

        gpio_register_set(GPIO_PIN_ADDR(ESP_INT), GPIO_PIN_INT_TYPE_SET(GPIO_PIN_INTR_DISABLE)
                | GPIO_PIN_PAD_DRIVER_SET(GPIO_PAD_DRIVER_DISABLE)
                | GPIO_PIN_SOURCE_SET(GPIO_AS_PIN_SOURCE));

        GPIO_REG_WRITE(GPIO_STATUS_W1TC_ADDRESS, 1 << ESP_INT);
        gpio_pin_intr_state_set(GPIO_ID_PIN(ESP_INT), GPIO_PIN_INTR_LOLEVEL);

        log("interrupts enabled");

        return ERR_OK;
}

struct netif* espenc_init(uint8_t *mac_addr, ip_addr_t *ip, ip_addr_t *mask, ip_addr_t *gw, bool dhcp) {
        ip_addr_t nulladdr;
        struct netif* new_netif;

        system_os_task(swint_Task, swint_TaskPrio, swint_TaskQueue, swint_TaskQueueLen);

        IP4_ADDR(&nulladdr, 0, 0, 0, 0);

        os_memcpy(enc_netif.hwaddr, mac_addr, 6);

        if(dhcp) {
                new_netif = netif_add(&enc_netif, &nulladdr, &nulladdr, &nulladdr, NULL, enc28j60_init, ethernet_input);
                if(new_netif) {
                        dhcp_start(new_netif);
                }
        } else {
                new_netif = netif_add(&enc_netif, ip, mask, gw, NULL, enc28j60_init, ethernet_input);
                if(new_netif) {
                        netif_set_up(new_netif);
                }
        }

        return new_netif;
}

#endif /* LWIP_HAVE_ENC28J60IF */
