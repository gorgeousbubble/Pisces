"""
auth.py — Basic Auth 中间件

外网访问保护：AUTH_ENABLED=True 时所有接口需要 HTTP Basic Auth。
内网部署可通过 AUTH_ENABLED=False 关闭。
"""

import secrets
from typing import Optional

from fastapi import Depends, HTTPException, status
from fastapi.security import HTTPBasic, HTTPBasicCredentials

import config

security = HTTPBasic(auto_error=False)


def require_auth(
    credentials: Optional[HTTPBasicCredentials] = Depends(security),
) -> None:
    """FastAPI 依赖项：验证 Basic Auth 凭据。"""
    if not config.AUTH_ENABLED:
        return  # 认证已关闭，直接放行

    if credentials is None:
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="Authentication required",
            headers={"WWW-Authenticate": "Basic"},
        )

    # 使用 secrets.compare_digest 防止时序攻击
    username_ok = secrets.compare_digest(
        credentials.username.encode(), config.AUTH_USERNAME.encode()
    )
    password_ok = secrets.compare_digest(
        credentials.password.encode(), config.AUTH_PASSWORD.encode()
    )

    if not (username_ok and password_ok):
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="Invalid credentials",
            headers={"WWW-Authenticate": "Basic"},
        )
