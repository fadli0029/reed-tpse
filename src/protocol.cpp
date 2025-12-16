#include "reed/protocol.hpp"

#include <sstream>

namespace reed {

uint8_t calculate_crc(const std::vector<uint8_t>& data) {
  uint32_t sum = 0;
  for (uint8_t b : data) {
    sum += b;
  }
  return static_cast<uint8_t>(sum & 0xFF);
}

std::vector<uint8_t> escape_data(const std::vector<uint8_t>& data) {
  std::vector<uint8_t> result;
  result.reserve(data.size() * 2);

  for (uint8_t b : data) {
    if (b == 0x5A) {
      result.push_back(0x5B);
      result.push_back(0x01);
    } else if (b == 0x5B) {
      result.push_back(0x5B);
      result.push_back(0x02);
    } else {
      result.push_back(b);
    }
  }

  return result;
}

std::vector<uint8_t> unescape_data(const std::vector<uint8_t>& data) {
  std::vector<uint8_t> result;
  result.reserve(data.size());

  for (size_t i = 0; i < data.size(); ++i) {
    if (data[i] == 0x5B && i + 1 < data.size()) {
      if (data[i + 1] == 0x01) {
        result.push_back(0x5A);
        ++i;
      } else if (data[i + 1] == 0x02) {
        result.push_back(0x5B);
        ++i;
      } else {
        result.push_back(data[i]);
      }
    } else {
      result.push_back(data[i]);
    }
  }

  return result;
}

std::vector<uint8_t> build_frame(const std::string& request_state,
                                 const std::string& cmd_type,
                                 const std::string& content,
                                 const std::string& version, int ack_number) {
  std::ostringstream body;

  // First line: REQUEST_STATE CMD_TYPE VERSION
  body << request_state << " " << cmd_type << " " << version << "\r\n";

  // Headers
  body << "ContentType=json\r\n";
  body << "ContentLength=" << content.size() << "\r\n";
  body << "AckNumber=" << ack_number << "\r\n";

  // Double CRLF separator + content
  body << "\r\n" << content;

  std::string message_body = body.str();

  // Total length = message length + 5 (overhead)
  uint16_t total_length = static_cast<uint16_t>(message_body.size() + 5);

  // Build data: length (2 bytes BE) + message
  std::vector<uint8_t> data_with_length;
  data_with_length.push_back(static_cast<uint8_t>((total_length >> 8) & 0xFF));
  data_with_length.push_back(static_cast<uint8_t>(total_length & 0xFF));

  for (char c : message_body) {
    data_with_length.push_back(static_cast<uint8_t>(c));
  }

  // Add CRC
  uint8_t crc = calculate_crc(data_with_length);
  data_with_length.push_back(crc);

  // Escape special bytes
  std::vector<uint8_t> escaped = escape_data(data_with_length);

  // Add frame markers
  std::vector<uint8_t> frame;
  frame.reserve(escaped.size() + 2);
  frame.push_back(FRAME_MARKER);
  frame.insert(frame.end(), escaped.begin(), escaped.end());
  frame.push_back(FRAME_MARKER);

  return frame;
}

std::optional<Response> parse_response(const std::vector<uint8_t>& data) {
  if (data.size() < 4) {
    return std::nullopt;
  }

  // Check frame markers
  if (data.front() != FRAME_MARKER || data.back() != FRAME_MARKER) {
    return std::nullopt;
  }

  // Unescape payload (between markers)
  std::vector<uint8_t> payload(data.begin() + 1, data.end() - 1);
  payload = unescape_data(payload);

  if (payload.size() < 3) {
    return std::nullopt;
  }

  // Skip length (2 bytes) and CRC (1 byte at end)
  std::string message(payload.begin() + 2, payload.end() - 1);

  Response response;
  response.raw = message;

  // Split headers and body
  size_t separator = message.find("\r\n\r\n");
  if (separator != std::string::npos) {
    std::string header_part = message.substr(0, separator);
    response.body = message.substr(separator + 4);

    // Try to parse body as JSON
    if (!response.body.empty()) {
      picojson::value v;
      std::string err = picojson::parse(v, response.body);
      if (err.empty()) {
        response.json = v;
      }
    }

    // Parse first line
    size_t first_line_end = header_part.find("\r\n");
    std::string first_line = (first_line_end != std::string::npos)
                                 ? header_part.substr(0, first_line_end)
                                 : header_part;

    // Extract version and status from first line
    std::istringstream iss(first_line);
    iss >> response.version >> response.status;
  }

  return response;
}

}  // namespace reed
