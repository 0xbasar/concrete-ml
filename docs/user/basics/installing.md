# Installing

## Python package

To install **Concrete ML** from PyPi, run the following:

```shell
pip install concrete-ml
```

```{note}
Note that **concrete-ml** has `pygraphviz` as an optional dependency to draw graphs.
```

```{WARNING}
`pygraphviz` requires `graphviz` packages being installed on your OS, see <a href="https://pygraphviz.github.io/documentation/stable/install.html">https://pygraphviz.github.io/documentation/stable/install.html</a>
```

```{DANGER}
`graphviz` packages are binary packages that won't automatically be installed by pip.
Do check <a href="https://pygraphviz.github.io/documentation/stable/install.html">https://pygraphviz.github.io/documentation/stable/install.html</a> for instructions on how to install `graphviz` for `pygraphviz`.
```

You can install the extra python dependencies for drawing with:

```shell
pip install concrete-ml[full]
# you may need to force reinstallation
pip install --force-reinstall concrete-ml[full]
```

## Docker image

```{WARNING}
FIXME: is there docker?
```

You can also get the **concrete-ml** docker image by either pulling the latest docker image or a specific version:

```shell
docker pull zamafhe/concrete-ml:latest
# or
docker pull zamafhe/concrete-ml:v0.2.0
```

The image can be used with docker volumes, [see the docker documentation here](https://docs.docker.com/storage/volumes/).

You can then use this image with the following command:

```shell
# Without local volume:
docker run --rm -it -p 8888:8888 zamafhe/concrete-ml:v0.2.0

# With local volume to save notebooks on host:
docker run --rm -it -p 8888:8888 -v /host/path:/data zamafhe/concrete-ml:v0.2.0
```

This will launch a **Concrete ML** enabled jupyter server in the docker, that you can access from your browser.

Alternatively, you can just open a shell in the docker with or without volumes:

```shell
docker run --rm -it zamafhe/concrete-ml:v0.2.0 /bin/bash
```
