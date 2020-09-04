defmodule FlutterEmbedder.PlatformChannelMessage do
  defstruct [:handle, :channel, :message]

  @type handle :: 0..255
  @type channel :: String.t()
  @type message :: binary()
  @type t() :: %__MODULE__{
          handle: handle(),
          channel: channel(),
          message: message()
        }

  @type ok_response() :: {:ok, binary()}
  @type error_response() :: {:error, binary()}
  @type not_implemented_response() :: :not_implemented

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

  @spec encode_response(t(), ok_response() | error_response() | not_implemented_response()) ::
          binary()
  def encode_response(%__MODULE__{handle: handle}, {:ok, value}) when is_binary(value) do
    <<handle::8, 0, value::binary>>
  end

  def encode_response(%__MODULE__{handle: handle}, {:error, value}) when is_binary(value) do
    <<handle::8, 1, value::binary>>
  end

  def encode_response(%__MODULE__{handle: handle}, :not_implemented) do
    <<handle::8>>
  end
end
