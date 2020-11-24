import Config

# Use shoehorn to start the main application. See the shoehorn
# docs for separating out critical OTP applications such as those
# involved with firmware updates.
bbb_init =
  if Mix.target() == :bbb do
    IO.puts("Enabling BBB stuff")

    [
      {:os, :cmd, ['modprobe pvrsrvkm']},
      {:os, :cmd, ['modprobe edt-ft5x06']},
      {:os, :cmd, ['pvrsrvctl --start --no-module']}
    ]
  else
    []
  end

config :shoehorn,
  init:
    bbb_init ++
      [
        :nerves_runtime,
        :nerves_pack,
        :nerves_key,
        :nerves_hub_link,
        {:os, :cmd, ['udevd -d']},
        {:os, :cmd, ['udevadm trigger --type=subsystems --action=add']},
        {:os, :cmd, ['udevadm trigger --type=devices --action=add']},
        {:os, :cmd, ['udevadm settle --timeout=30']}
      ],
  app: Mix.Project.config()[:app]

# Nerves Runtime can enumerate hardware devices and send notifications via
# SystemRegistry. This slows down startup and not many programs make use of
# this feature.

config :nerves_runtime, :kernel, use_system_registry: false

# Erlinit can be configured without a rootfs_overlay. See
# https://github.com/nerves-project/erlinit/ for more information on
# configuring erlinit.

config :nerves,
  erlinit: [
    hostname_pattern: "flutter-demo-%s",
    ctty: "ttyAMA0"
  ]

config :nerves_hub_cli, org: "smartrent-flutter"

config :nerves_hub_link,
  fwup_public_keys: [
    # flutter_demo_key
    "mHkOXo99PW5vIcQTRZy1ed0Zs1ahP80PiPMe3OUfGcY="
  ],
  nerves_key: [i2c_bus: 1],
  org: "smartrent-flutter",
  remote_iex: true,
  socket: [
    reconnect_interval: 10_000
  ]

# Authorize the device to receive firmware using your public key.
# See https://hexdocs.pm/nerves_firmware_ssh/readme.html for more information
# on configuring nerves_firmware_ssh.

keys =
  [
    Path.join([System.user_home!(), ".ssh", "id_rsa.pub"]),
    Path.join([System.user_home!(), ".ssh", "id_ecdsa.pub"]),
    Path.join([System.user_home!(), ".ssh", "id_ed25519.pub"])
  ]
  |> Enum.filter(&File.exists?/1)
  |> Enum.map(&File.read!/1)

if keys == [],
  do:
    Mix.raise("""
    No SSH public keys found in ~/.ssh. An ssh authorized key is needed to
    log into the Nerves device and update firmware on it using ssh.
    See your project's config.exs for this error message.
    """)

config :nerves_ssh,
  authorized_keys:
    [
      "ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAACAQD17lo684Xufpqi2H7he0akr5S9O2MxxjLxUwgET3F7RsD8Urhm+xhpnSRkjHFeCOu2eZNWDZFx28ax+44teZqaofpVHnuEKJkSocqx5lZt27ttDIYQPEj7AQGOgkVSH+DBJK9DZDJxNDkaYAgoEIYVcCRCTQtBJJnPdYK9DDQES884xhRIwwDLYFV7PBsYD1h9UQZ16b8Y+WrWzCOzdW/d0+gzf9T/3FdoVzsm/vza+wjGdMhglETSesNUC5HbUVLZ8IbPMizn/+mPMiql5pDxA7fJWlVeX4EcjIAdfzqkonv5VTuF63Gfx5yltgCH7aE7tPBq3mPZJyEsaC1WGRJJlc1LiwxSkZoUgZ3b1ggOjv6M0QmxiJfqLnhhvUYwtr//ANiqK1BLfiC916W3pSKH+VSCu4Pantpun3mgkf4UBYVqGrZ5jPVj/NJqVzaFqZ9WjOwYOQpQpmuA7zlKArPRdDtW3Tk04qnY6hW2xyyp/X7QjNFJNBiaucX4BT/LKfYbmEabo2NxANNltnAUCKGJ9/pi/NTdrV+3gaD7RCMz84TF/xOpH0q2yEyJUsid4MV7OdQkuBiJ2np/0HxwCokm8rkbyxwqJfasPfOhVQUyMNN/8p4H+wPInuVlH0CeInZSb/coAKX6qiQ2sA53vgVfkltlVol9xFmxl9+VEQ7Saw== Smartrent Hub Shared SSH key"
    ] ++ keys

# `:tcpip_tunnel_in` lets you connect to the Flutter Observatory. Run
# `ssh -L 38907:localhost:38907 nerves-0105.local` to forward the port out
# and replace the port number with whatever port Flutter chooses.
config :nerves_ssh,
  authorized_keys: keys,
  daemon_option_overrides: [{:tcpip_tunnel_in, true}]

# Configure the network using vintage_net
# See https://github.com/nerves-networking/vintage_net for more information
config :vintage_net,
  regulatory_domain: "US",
  config: [
    {"usb0", %{type: VintageNetDirect}},
    {"eth0",
     %{
       type: VintageNetEthernet,
       ipv4: %{method: :dhcp}
     }},
    {"wlan0", %{type: VintageNetWiFi, vintage_net_wifi: %{}}}
  ]

config :mdns_lite,
  # The `host` key specifies what hostnames mdns_lite advertises.  `:hostname`
  # advertises the device's hostname.local. For the official Nerves systems, this
  # is "nerves-<4 digit serial#>.local".  mdns_lite also advertises
  # "nerves.local" for convenience. If more than one Nerves device is on the
  # network, delete "nerves" from the list.

  host: [:hostname, "flutter-demo"],
  ttl: 120,

  # Advertise the following services over mDNS.
  services: [
    %{
      name: "SSH Remote Login Protocol",
      protocol: "ssh",
      transport: "tcp",
      port: 22
    },
    %{
      name: "Secure File Transfer Protocol over SSH",
      protocol: "sftp-ssh",
      transport: "tcp",
      port: 22
    },
    %{
      name: "Erlang Port Mapper Daemon",
      protocol: "epmd",
      transport: "tcp",
      port: 4369
    }
  ]

# Import target specific config. This must remain at the bottom
# of this file so it overrides the configuration defined above.
# Uncomment to use target specific configurations

# import_config "#{Mix.target()}.exs"
