# FlutterEmbedder

[Flutter](https://flutter.dev) is an open-source UI software development kit
created by Google. This project aims to use the Flutter "embedder" system to
provide embedded UI applications targeting Nerves devices.

## Similar Projects

This idea isn't completely original. Some notable similar projects include:

* [flutter-pi](https://github.com/ardera/flutter-pi) - A light-weight Flutter Engine Embedder for Raspberry Pi that runs without X.
* [go-flutter](https://github.com/go-flutter-desktop/go-flutter) - Flutter on Windows, MacOS and Linux - based on Flutter Embedding, Go and GLFW.
* [Flutter from Scratch](https://medium.com/flutter/flutter-on-raspberry-pi-mostly-from-scratch-2824c5e7dcb1) - A really nice introduction to the Flutter Embedder.

## Getting Started

> NOTE: this is still very much a work in progress project and document.

The below document assumes you use something akin to [Poncho Projects](https://hexdocs.pm/nerves/1.3.2/user-interfaces.html#choosing-a-project-structure)
when developing your Nerves application. This guide will take you through
creating both the `nerves` and `flutter` application codebases, but adding
`flutter` to an existing app should be equally as easy.

### Dependencies and setup

The first steps will be to install Nerves and Flutter. You can find guides to
both below:

* [Nerves](https://hexdocs.pm/nerves/1.3.2/installation.html#content)
* [Flutter](https://flutter.dev/docs/get-started/install)

Once that's complete create a new folder for your project and then scafold two
applications:

```bash
mkdir flutter-nerves-helloworld
cd flutter-nerves-helloworld
# Create a new Nerves Project
# currently, only rpi3 is supported.
mix nerves.new firmware --target=rpi3
# Create a new flutter application
flutter create ui
```

### Wiring it up

The first step to getting up and running is to add `flutter_embedder` to your
Elixir application's dependencies. Open `mix.exs`:

```elixir
def deps do
  # ... ommited for clarity
  # Dependencies for specific targets
  # Add this line
  {:flutter_embedder, "~> 0.0", targets: :rpi3}
  # modify the `nerves_system_rpi3` line:
  {:drm_system_rpi3, "~> 1.12", runtime: false, targets: :rpi3}
end
```

Then run a quick `mix deps.get` in that folder. This will fetch the
flutter_embedder project.

Next, in that same file add a new option to the `def project` section:

```elixir
def project do
  # ... ommited for clarity
  # add a new line to configure elixir to run the `flutter` compile step:
  compilers: compilers(Mix.target()),
  # Configure the flutter compiler to compile the app we created
  # in the previous step
  flutter: [
    cd: Path.expand("../ui", __DIR__)
  ]
end

# Create this function:
def compilers(:rpi3), do: [:flutter | Mix.compilers()]
def compilers(:host), do: Mix.compilers()
```

The final step is to start the Flutter embedder in your application. The easiest
way to do this is by adding the child to your supervision tree. Open
`lib/application.ex`:

```elixir
def children(_target) do
  flutter_opts = [
    flutter_assets: Application.app_dir(:firmware, ["priv", "flutter_assets"])
  ]
  [
    # ... ommited for clarity
    {FlutterEmbedder, flutter_opts}
  ]
end
```

Finally, to get the application up and running, follow the standard Nerves
workflow:

```bash
mix firmware.burn
```

And when it boots, you should see the default flutter application.
