# ESP32 Dual ADC RMS Meter with Wi-Fi Provisioning and HTTP API

This project implements a **dual-channel RMS measurement system** on an ESP32,
combined with **Wi-Fi provisioning**, a **local HTTP API**, and a **captive
portal–assisted setup flow**.

It is designed for **local network use**, educational purposes, and integration
into home or lab environments where ADC measurements need to be accessed over
Wi-Fi.

---

## Features

- Dual ADC input sampling on the ESP32
- RMS value computation over a configurable sample window
- Persistent Wi-Fi provisioning via SoftAP
- Captive-portal–assisted setup (with manual fallback)
- Local HTTP API for retrieving measurement data
- Works without cloud services or external dependencies
- Access via either:
  - the provisioning access point, or
  - the home Wi-Fi network after configuration

---

## How it works (high level)

1. On first boot (or if no Wi-Fi credentials are stored), the ESP32:
   - starts a **SoftAP** named `JAK_DEVICE_*`
   - exposes a **provisioning web interface**

2. The user connects to the AP and enters home Wi-Fi credentials.

3. The device stores the credentials and connects to the home network.

4. From that point on:
   - the ESP32 is reachable via its **router-assigned IP**
   - the provisioning AP remains available for reconfiguration at any time

---

## Wi-Fi provisioning

- **SSID:** `JAK_DEVICE_*`
- **Default password:** `configureme`
- **Provisioning IP:** `http://192.168.4.1`

All of the above can be changed in `app_config.h`.

A captive portal is used to help devices automatically open the provisioning
page. If it does not open automatically, simply browse to:

http://192.168.4.1


---

## HTTP API

Once connected to the home Wi-Fi network, the ESP32 exposes an HTTP API that
allows clients on the same network to retrieve measurement data.

The device IP is printed by the API after connection.

> Note: The API is intended for use on trusted local networks and does not
> implement authentication or encryption.

---

## Security considerations

This project includes **basic, intentional safety measures**, but it is **not
designed as a hardened or enterprise-grade secure device**.

Implemented measures:
- WPA2-protected provisioning access point
- Local-network–only access (no cloud or WAN exposure)
- No external services or background connections

Not implemented (by design):
- HTTPS / TLS
- API authentication or user accounts
- Payload encryption
- Brute-force protection

If stronger security is required, additional measures should be implemented by
the user.

---

## Configuration

All user-adjustable parameters are centralized in:

app_config.h


This includes:
- provisioning SSID prefix
- provisioning password
- SoftAP IP address
- ADC parameters
- sampling rates and window sizes

---

## Intended use

This project is well suited for:
- learning ESP32 networking and ADC usage
- lab instrumentation
- local monitoring tools
- home automation experiments
- embedded systems tutorials

It is **not** intended for direct internet exposure without additional
hardening.

---

## License

No license is currently specified.  
Usage, modification, and redistribution are subject to the repository owner’s
terms.

---

## Related documentation

This repository accompanies a detailed tutorial explaining:
- the ADC measurement pipeline
- RMS computation
- Wi-Fi provisioning flow
- captive portal behavior
- HTTP API usage

📖 **Full tutorial:**  
https://jak-services.github.io/en/tutorial_esp32_adc_wifi_api.html

---

## License

This project is released into the public domain under The Unlicense.


