export class ServiceError extends Error {
  constructor(code, message, status = 502, providerStatus = undefined) {
    super(message);
    this.name = "ServiceError";
    this.code = code;
    this.status = status;
    this.providerStatus = providerStatus;
  }
}

export function abortError(signal, fallbackCode = "REQUEST_CANCELLED") {
  if (signal?.reason instanceof ServiceError) return signal.reason;
  return new ServiceError(fallbackCode, "Yêu cầu đã bị hủy.", 499);
}

export function publicError(error) {
  if (error instanceof ServiceError) {
    const value = { code: error.code, message: error.message };
    if (Number.isInteger(error.providerStatus)) value.provider_status = error.providerStatus;
    return { status: error.status, value: { error: value } };
  }
  return {
    status: 502,
    value: { error: { code: "AI_GATEWAY_ERROR", message: "Dịch vụ AI gặp lỗi không xác định." } },
  };
}
