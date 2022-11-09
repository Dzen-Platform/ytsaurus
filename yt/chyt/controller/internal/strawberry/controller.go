package strawberry

import (
	"context"

	"a.yandex-team.ru/library/go/core/log"
	"a.yandex-team.ru/yt/go/ypath"
	"a.yandex-team.ru/yt/go/yson"
	"a.yandex-team.ru/yt/go/yt"
)

// Controller encapsulates particular application business logic, in particular:
// how operations should be started, which files to bring with them, how to check liveness, etc.
type Controller interface {
	// Prepare builds all necessary operation spec fields.
	Prepare(ctx context.Context, oplet *Oplet) (
		spec map[string]interface{}, description map[string]interface{}, annotation map[string]interface{}, err error)

	// Family returns short lowercase_with_underscore identifier which is included to all vanilla
	// operation annotations started by this strawberry controller. This allows efficient operation
	// filtering using YT list_operations API.
	Family() string

	ParseSpeclet(specletYson yson.RawValue) (any, error)

	// TryUpdate tries to update controller, returns true if controller has changed.
	TryUpdate() (bool, error)
}

type ControllerFactory = func(l log.Logger, ytc yt.Client, root ypath.Path, cluster string, config yson.RawValue) Controller
