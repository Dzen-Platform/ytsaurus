package app

import (
	"context"
	"fmt"

	"a.yandex-team.ru/library/go/core/log"
	"a.yandex-team.ru/yt/chyt/controller/internal/strawberry"
	"a.yandex-team.ru/yt/go/ypath"
	"a.yandex-team.ru/yt/go/yt"
	"a.yandex-team.ru/yt/go/yt/ythttp"
	"a.yandex-team.ru/yt/go/yterrors"
)

type ClusterInitializerConfig struct {
	BaseConfig

	// RobotUsername is the name of the robot from which all controller operations are done.
	RobotUsername string `yson:"robot_username"`
}

type ClusterInitializer struct {
	ytc                   yt.Client
	l                     log.Logger
	config                ClusterInitializerConfig
	strawberryInitializer strawberry.ClusterInitializer
}

func NewClusterInitializer(config *ClusterInitializerConfig, initializerFactory strawberry.ClusterInitializerFactory) (initializer ClusterInitializer) {
	l := newLogger("init_cluster", false /*stderr*/)
	initializer.l = l
	initializer.config = *config

	config.Token = getStrawberryToken(config.Token)

	var err error
	initializer.ytc, err = ythttp.NewClient(&yt.Config{
		Token:  config.Token,
		Proxy:  initializer.config.Proxy,
		Logger: withName(l, "yt"),
	})
	if err != nil {
		panic(err)
	}
	initializer.strawberryInitializer = initializerFactory(l, initializer.ytc)
	return
}

func (initializer *ClusterInitializer) checkRobotPermissions(ctx context.Context) error {
	if initializer.config.RobotUsername == "" {
		return nil
	}
	userPath := ypath.Path("//sys/users").Child(initializer.config.RobotUsername)
	ok, err := initializer.ytc.NodeExists(ctx, userPath, nil)
	if err != nil {
		return err
	}
	if !ok {
		return yterrors.Err(fmt.Sprintf("user %v does not exist", initializer.config.RobotUsername))
	}

	paths := []ypath.Path{
		initializer.config.StrawberryRoot,
		ypath.Path("//sys/access_control_object_namespaces").Child(initializer.strawberryInitializer.ACONamespace()),
		ypath.Path("//sys/schemas/access_control_object"),
	}
	permissions := []yt.Permission{yt.PermissionCreate, yt.PermissionRead}

	for _, path := range paths {
		for _, permission := range permissions {
			response, err := initializer.ytc.CheckPermission(
				ctx,
				initializer.config.RobotUsername,
				permission,
				path,
				nil)
			if err != nil {
				return err
			}
			if response.Action != yt.ActionAllow {
				return yterrors.Err(fmt.Sprintf("robot has no permission to %v in %v", permission, path))
			}
		}
	}

	return nil
}

func (initializer *ClusterInitializer) createRootIfNotExists(ctx context.Context) error {
	_, err := initializer.ytc.CreateNode(ctx, initializer.config.StrawberryRoot, yt.NodeMap, &yt.CreateNodeOptions{
		Recursive:      true,
		IgnoreExisting: true,
	})
	return err
}

func (initializer *ClusterInitializer) createACONamespaceIfNotExists(ctx context.Context) error {
	_, err := initializer.ytc.CreateObject(ctx, yt.NodeAccessControlObjectNamespace, &yt.CreateObjectOptions{
		Attributes: map[string]any{
			"name": initializer.strawberryInitializer.ACONamespace(),
			"idm_roles": map[string]any{
				"manage": map[string]any{
					"idm_name": "Manage",
					"permissions": []yt.Permission{
						yt.PermissionRead,
						yt.PermissionManage,
						yt.PermissionRemove,
					},
				},
				"use": map[string]any{
					"idm_name": "Use",
					"permissions": []yt.Permission{
						yt.PermissionUse,
					},
				},
			},
		},
		IgnoreExisting: true,
	})
	return err
}

func (initializer *ClusterInitializer) InitCluster() error {
	ctx := context.Background()
	if err := initializer.createRootIfNotExists(ctx); err != nil {
		return err
	}
	if err := initializer.createACONamespaceIfNotExists(ctx); err != nil {
		return err
	}
	if err := initializer.strawberryInitializer.InitializeCluster(); err != nil {
		return err
	}
	if err := initializer.checkRobotPermissions(ctx); err != nil {
		return err
	}
	return nil
}
