# Sample T2 App for osal-zephyr

This repository is the T2 application repository for https://github.com/yashi/osal-zephyr .

## How to build and run

If you have already built [the Hello World sample][1] in Zephyr, you
can build and run this sample with the following steps.

[1]: https://docs.zephyrproject.org/latest/samples/hello_world/README.html

```
mkdir workspace
cd workspace
python3 -m venv .venv
source .venv/bin/activate
pip install west
west init -m https://github.com/yashi/osal-zephyr-sample
west update
pip install -r zephyr/scripts/requirements.txt
west build -b qemu_cortex_m3 -t run osal-zephyr-sample
```
