CREATE TABLE `gateway_proxy_basic_auth` (
     `id` bigint unsigned NOT NULL AUTO_INCREMENT COMMENT '自增id',
     `username` varchar(128)  NOT NULL DEFAULT '' COMMENT '域名',
     `password` varchar(128) NOT NULL DEFAULT '' COMMENT '账号',
     `bind_type` int(2) NOT NULL DEFAULT '0' COMMENT '0-用户，1-psm',
     `bind_name` varchar(128) NOT NULL DEFAULT '' COMMENT '绑定的用户名或psm名称',
     `create_time` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT '创建时间',
     `update_time` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT '更新时间',
     PRIMARY KEY (`id`),
     UNIQUE KEY `uk_domain` (`username`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='basic auth 配置'
