defmodule FlutterEmbedder.StandardCall do
  alias FlutterEmbedder.StandardCallCodec
  defstruct [:handle, :channel, :method, :args]

  def decode(
        <<handle::8, channel_length::little-16, channel::binary-size(channel_length),
          message_length::little-16, message::binary-size(message_length)>>
      ) do
    {method, args_} = StandardCallCodec.decode_value(message)
    {args, ""} = StandardCallCodec.decode_value(args_)

    %__MODULE__{
      handle: handle,
      channel: channel,
      method: method,
      args: args
    }
  end
end
