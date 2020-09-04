defmodule FlutterEmbedder.PlatformChannelMessage do
  defstruct [:handle, :channel, :message]

  @type t() :: %__MODULE__{
          handle: 0..255,
          channel: String.t(),
          message: String.t()
        }

  @type ok_response() :: {:ok, binary()}
  @type error_response() :: {:error, binary()}

  @spec decode(binary()) :: t()
  def decode(
        <<handle::8, channel_length::little-16, channel::binary-size(channel_length),
          message_length::little-16, message::binary-size(message_length)>>
      ) do
    %__MODULE__{
      handle: handle,
      channel: channel,
      message: message
    }
  end

  @spec encode_response(t(), ok_response() | error_response()) :: binary()
  def encode_response(%__MODULE__{handle: handle}, {:ok, value}) when is_binary(value) do
    <<handle::8, 0, value::binary>>
  end

  def encode_response(%__MODULE__{handle: handle}, {:error, value}) when is_binary(value) do
    <<handle::8, 0, value::binary>>
  end
end
